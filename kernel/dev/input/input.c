// LikeOS input core - implementation
//
// Owns every input device's state and turns what drivers report into the
// packets the event-device layer hands to readers.  See input.h for the
// interface and the context rules.
//
// Event processing.  input_event() decides, under the device's event
// spinlock, what an event means:
//   - not declared by the device: dropped;
//   - EV_KEY / EV_SW / EV_LED: passed only when it changes the state (a key
//     value of 2, autorepeat, always passes for a key the device has and
//     changes no state);
//   - EV_ABS: the fuzz filter is applied against the last value, and an
//     unchanged value is dropped.  For the per-slot multitouch axes "last
//     value" is the cached value of the current slot; ABS_MT_SLOT itself is
//     only staged, and emitted in front of the first real change to that
//     slot, so readers never see a slot switch with nothing behind it;
//   - EV_REL: passed when non-zero;
//   - EV_MSC / EV_SND / EV_REP / EV_FF / EV_PWR: passed, and also given to
//     the driver's event() callback (as LED changes are) while the device
//     is open;
//   - EV_SYN / SYN_REPORT: ends the packet; a packet holding nothing but the
//     SYN_REPORT is never handed on.
// Accepted events collect in dev->vals.  The packet goes to the event-device
// layer at SYN_REPORT, or -- with a SYN_REPORT the core adds -- when the
// buffer is about to overflow (a driver reporting more per packet than its
// declared capabilities suggest).  Both parts of such a split packet carry
// the same timestamp.
//
// Software autorepeat: a device with EV_REP whose driver did not preset the
// timings gets the core's (input_enable_softrepeat): a key press passed to
// the readers arms a delayed work item that, while the key stays down,
// reports it again with value 2 after REP_DELAY and then every REP_PERIOD;
// a release (of any key) or the next press stops or re-aims it.
//
// In-kernel listeners: an input_handler is offered every device that
// registers (connect) and attaches input_handles; open handles get each
// packet right after the event node, under the event lock.  Devices offer
// themselves to the handlers once their node exists, handlers offer
// themselves to the devices already there -- both under
// g_input_handler_lock, with dev->handlers_attached telling which devices
// the handler walk must not offer twice.  A device going away first takes
// no new users, then asks every handler to detach (disconnect), then loses
// its node.
//
// Locking:
//   g_input_handler_lock  the handler list, offering devices to handlers,
//                         detaching them (a mutex: connect and disconnect
//                         sleep)
//   g_input_list_lock     the registered-device list and the inputN numbers
//   dev->mutex            users / kernel_users, open()/close() callbacks,
//                         attaching and detaching handles (may sleep)
//   dev->event_lock       the device's state and packet, the handle list as
//                         the event path sees it (any context)
// Order: g_input_handler_lock -> dev->mutex -> g_input_list_lock ->
//        dev->event_lock -> evdev locks.
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) 1999-2002 Vojtech Pavlik

#include <kernel/dev/input/input.h>
#include <kernel/dev/input/input-mt.h>
#include <kernel/dev/input/evdev.h>
#include <kernel/fs/devevent.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h> /* errno values */
#include <kernel/ke/timer.h>

/* Room for inputN numbers; far more than a machine has input devices. */
#define INPUT_MAX_NUMBERS 256

static spinlock_t g_input_list_lock = SPINLOCK_INIT("input_list");
static struct input_dev *g_input_head; // registration order
static unsigned long g_input_numbers[INPUT_BITS_TO_LONGS(INPUT_MAX_NUMBERS)];

static DEFINE_MUTEX(g_input_handler_lock);
static struct input_handler *g_input_handlers; // registration order

// ---------------------------------------------------------------------------
// Allocation and references
// ---------------------------------------------------------------------------

static void input_dev_init_common(struct input_dev *dev)
{
	spinlock_init(&dev->event_lock, "input_event");
	mutex_init(&dev->mutex, "input_dev");
	refcount_set(&dev->refcount, 1);
	dev->input_no = -1;
}

struct input_dev *input_allocate_device(void)
{
	struct input_dev *dev;

	might_sleep();
	dev = (struct input_dev *)kcalloc(1, sizeof(*dev));
	if (!dev)
		return NULL;
	input_dev_init_common(dev);
	return dev;
}

void input_get_device(struct input_dev *dev)
{
	if (dev)
		refcount_inc(&dev->refcount);
}

void input_put_device(struct input_dev *dev)
{
	if (!dev || !refcount_dec_and_test(&dev->refcount))
		return;
	/* The built-in devices are never released; reaching zero on one is
	 * an unbalanced put somewhere. */
	if (WARN_ON(dev->static_storage))
		return;
	WARN_ON(dev->registered);
	input_mt_destroy_slots(dev);
	kfree(dev->vals);
	dev->vals = NULL;
	kfree(dev);
}

void input_free_device(struct input_dev *dev)
{
	if (!dev)
		return;
	/* A registered device is taken away with input_unregister_device(),
	 * which drops this same reference; doing both would free it twice. */
	if (WARN_ON(dev->registered))
		return;
	input_put_device(dev);
}

// ---------------------------------------------------------------------------
// Capabilities
// ---------------------------------------------------------------------------

void input_set_capability(struct input_dev *dev, unsigned int type,
			  unsigned int code)
{
	unsigned long *map;
	unsigned int max;

	if (!dev)
		return;
	switch (type) {
	case EV_KEY:
		map = dev->keybit, max = KEY_MAX;
		break;
	case EV_REL:
		map = dev->relbit, max = REL_MAX;
		break;
	case EV_ABS:
		map = dev->absbit, max = ABS_MAX;
		break;
	case EV_MSC:
		map = dev->mscbit, max = MSC_MAX;
		break;
	case EV_SW:
		map = dev->swbit, max = SW_MAX;
		break;
	case EV_LED:
		map = dev->ledbit, max = LED_MAX;
		break;
	case EV_SND:
		map = dev->sndbit, max = SND_MAX;
		break;
	case EV_FF:
		map = dev->ffbit, max = FF_MAX;
		break;
	case EV_PWR:
	case EV_REP:
		/* No codes of their own to declare. */
		map = NULL, max = 0;
		break;
	default:
		WARN(1, "input_set_capability: unknown event type %u\n", type);
		return;
	}
	if (map) {
		if (WARN_ON(code > max))
			return;
		input_set_bit(code, map);
	}
	input_set_bit(type, dev->evbit);
}

void input_set_abs_params(struct input_dev *dev, unsigned int axis, int min,
			  int max, int fuzz, int flat)
{
	struct input_absinfo *a;
	uint64_t f;

	if (!dev || WARN_ON(axis >= ABS_CNT))
		return;
	/* Under the event lock: a driver may adjust a range on a registered
	 * device, and a packet must not see half of the change. */
	spin_lock_irqsave(&dev->event_lock, &f);
	a = &dev->absinfo[axis];
	a->minimum = min;
	a->maximum = max;
	a->fuzz = fuzz;
	a->flat = flat;
	input_set_bit(axis, dev->absbit);
	input_set_bit(EV_ABS, dev->evbit);
	spin_unlock_irqrestore(&dev->event_lock, f);
}

void input_copy_abs(struct input_dev *dst, unsigned int dst_axis,
		    const struct input_dev *src, unsigned int src_axis)
{
	if (!dst || !src || WARN_ON(dst_axis >= ABS_CNT || src_axis >= ABS_CNT))
		return;
	/* The source must have the axis. */
	if (WARN_ON(!(input_test_bit(EV_ABS, src->evbit) &&
		      input_test_bit(src_axis, src->absbit))))
		return;
	input_set_capability(dst, EV_ABS, dst_axis);
	dst->absinfo[dst_axis] = src->absinfo[src_axis];
}

// ---------------------------------------------------------------------------
// Event processing (event_lock held)
// ---------------------------------------------------------------------------

/* The fuzz filter: a value within half the fuzz of the previous one is
 * noise and reads as the previous value; within the fuzz it moves a
 * quarter of the way, within twice the fuzz half the way; beyond that it
 * is taken as it is.  (The sums are done in 64 bits: the same results for
 * every value that does not overflow 32 bits.) */
static int input_defuzz_abs_event(int value, int old_val, int fuzz)
{
	int64_t v = value, o = old_val, fz = fuzz;

	if (fz) {
		if (v > o - fz / 2 && v < o + fz / 2)
			return old_val;
		if (v > o - fz && v < o + fz)
			return (int)((o * 3 + v) / 4);
		if (v > o - fz * 2 && v < o + fz * 2)
			return (int)((o + v) / 2);
	}
	return value;
}

static void input_start_autorepeat(struct input_dev *dev, unsigned int code)
{
	if (input_test_bit(EV_REP, dev->evbit) && dev->rep[REP_PERIOD] &&
	    dev->rep[REP_DELAY] && dev->softrepeat && dev->repeat_live &&
	    system_wq) {
		dev->repeat_key = code;
		mod_delayed_work(system_wq, &dev->repeat_work,
				 msecs_to_jiffies((unsigned int)dev->rep[REP_DELAY]));
	}
}

static void input_stop_autorepeat(struct input_dev *dev)
{
	if (dev->softrepeat)
		cancel_delayed_work(&dev->repeat_work);
}

/* Hand the packet in dev->vals to the readers: the event node, then -- unless
 * a client of the node holds a grab, which makes the device its own -- every
 * open in-kernel handle; then the key presses and releases in it aim the
 * software autorepeat.  event_lock held. */
static void input_pass_values(struct input_dev *dev)
{
	struct input_handle *h;
	unsigned int i;
	int grabbed = 0;

	if (!dev->num_vals)
		return;
	if (dev->evdev || dev->handles) {
		/* The packet's time: the driver's, or now.  It stays until
		 * the packet's SYN_REPORT, so both halves of a packet split
		 * for lack of room carry the same one. */
		if (!dev->timestamp_set) {
			dev->timestamp_ns = timer_get_precise_ns();
			dev->timestamp_set = 1;
		}
		if (dev->evdev)
			grabbed = evdev_pass_values(dev->evdev, dev->vals,
						    dev->num_vals,
						    dev->timestamp_ns);
		for (h = dev->handles; h && !grabbed; h = h->next)
			if (h->open && h->events)
				h->events(h, dev->vals, dev->num_vals,
					  dev->timestamp_ns);
	}

	if (input_test_bit(EV_REP, dev->evbit) &&
	    input_test_bit(EV_KEY, dev->evbit)) {
		for (i = 0; i < dev->num_vals; i++) {
			const struct input_value *v = &dev->vals[i];

			if (v->type == EV_KEY && v->value != 2) {
				if (v->value)
					input_start_autorepeat(dev, v->code);
				else
					input_stop_autorepeat(dev);
			}
		}
	}
}

/* What input_get_disposition() decided about one event. */
#define INPUT_IGNORE_EVENT 0x00
#define INPUT_PASS_TO_HANDLERS 0x01 // into the packet for readers
#define INPUT_PASS_TO_DEVICE 0x02 // to the driver's event() callback
#define INPUT_SLOT 0x04 // emit the staged ABS_MT_SLOT first
#define INPUT_FLUSH 0x08 // end of packet
#define INPUT_PASS_TO_ALL (INPUT_PASS_TO_HANDLERS | INPUT_PASS_TO_DEVICE)

static int input_handle_abs_event(struct input_dev *dev, unsigned int code,
				  int *pval)
{
	struct input_mt *mt = dev->mt;
	int is_new_slot = 0;
	int is_mt_event;
	int *pold;

	if (code == ABS_MT_SLOT) {
		/* Staged only: it reaches readers in front of the first
		 * change to that slot (INPUT_SLOT below). */
		if (mt && *pval >= 0 && *pval < mt->num_slots)
			mt->slot = *pval;
		return INPUT_IGNORE_EVENT;
	}

	is_mt_event = input_is_mt_value((int)code);
	if (!is_mt_event) {
		pold = &dev->absinfo[code].value;
	} else if (mt) {
		pold = &mt->slots[mt->slot].abs[code - INPUT_MT_FIRST];
		is_new_slot = mt->slot != dev->absinfo[ABS_MT_SLOT].value;
	} else {
		/* Anonymous multitouch contacts (SYN_MT_REPORT protocol):
		 * there is no per-contact state to filter against. */
		pold = NULL;
	}

	if (pold) {
		*pval = input_defuzz_abs_event(*pval, *pold,
					       dev->absinfo[code].fuzz);
		if (*pold == *pval)
			return INPUT_IGNORE_EVENT;
		*pold = *pval;
	}

	/* A real change to a slot other than the one readers last heard
	 * about: tell them which slot first. */
	if (is_new_slot) {
		dev->absinfo[ABS_MT_SLOT].value = mt->slot;
		return INPUT_PASS_TO_HANDLERS | INPUT_SLOT;
	}
	return INPUT_PASS_TO_HANDLERS;
}

static int input_get_disposition(struct input_dev *dev, unsigned int type,
				 unsigned int code, int *pval)
{
	int disposition = INPUT_IGNORE_EVENT;
	int value = *pval;

	switch (type) {
	case EV_SYN:
		switch (code) {
		case SYN_CONFIG:
			disposition = INPUT_PASS_TO_ALL;
			break;
		case SYN_REPORT:
			disposition = INPUT_PASS_TO_HANDLERS | INPUT_FLUSH;
			break;
		case SYN_MT_REPORT:
			disposition = INPUT_PASS_TO_HANDLERS;
			break;
		}
		/* SYN_DROPPED and the rest are the event-device layer's to
		 * generate, never a driver's. */
		break;

	case EV_KEY:
		if (input_code_supported(code, dev->keybit, KEY_MAX)) {
			/* Autorepeat changes no state. */
			if (value == 2) {
				disposition = INPUT_PASS_TO_HANDLERS;
				break;
			}
			if (input_test_bit(code, dev->key) != !!value) {
				if (value)
					input_set_bit(code, dev->key);
				else
					input_clear_bit(code, dev->key);
				disposition = INPUT_PASS_TO_HANDLERS;
			}
		}
		break;

	case EV_SW:
		if (input_code_supported(code, dev->swbit, SW_MAX) &&
		    input_test_bit(code, dev->sw) != !!value) {
			if (value)
				input_set_bit(code, dev->sw);
			else
				input_clear_bit(code, dev->sw);
			disposition = INPUT_PASS_TO_HANDLERS;
		}
		break;

	case EV_ABS:
		if (input_code_supported(code, dev->absbit, ABS_MAX))
			disposition = input_handle_abs_event(dev, code, &value);
		break;

	case EV_REL:
		if (input_code_supported(code, dev->relbit, REL_MAX) && value)
			disposition = INPUT_PASS_TO_HANDLERS;
		break;

	case EV_MSC:
		if (input_code_supported(code, dev->mscbit, MSC_MAX))
			disposition = INPUT_PASS_TO_ALL;
		break;

	case EV_LED:
		if (input_code_supported(code, dev->ledbit, LED_MAX) &&
		    input_test_bit(code, dev->led) != !!value) {
			if (value)
				input_set_bit(code, dev->led);
			else
				input_clear_bit(code, dev->led);
			disposition = INPUT_PASS_TO_ALL;
		}
		break;

	case EV_SND:
		if (input_code_supported(code, dev->sndbit, SND_MAX)) {
			/* The tone's state follows the value, and every
			 * request is passed on: a new frequency is a change
			 * too. */
			if (value)
				input_set_bit(code, dev->snd);
			else
				input_clear_bit(code, dev->snd);
			disposition = INPUT_PASS_TO_ALL;
		}
		break;

	case EV_REP:
		if (code <= REP_MAX && value >= 0 && dev->rep[code] != value) {
			dev->rep[code] = value;
			disposition = INPUT_PASS_TO_ALL;
		}
		break;

	case EV_FF:
		if (value >= 0)
			disposition = INPUT_PASS_TO_ALL;
		break;

	case EV_PWR:
		disposition = INPUT_PASS_TO_ALL;
		break;
	}

	*pval = value;
	return disposition;
}

static void input_append(struct input_dev *dev, unsigned int type,
			 unsigned int code, int value)
{
	struct input_value *v;

	if (!dev->vals)
		return; // not registered: state is kept, nothing to hand on
	if (WARN_ON_ONCE(dev->num_vals >= dev->max_vals))
		return;
	v = &dev->vals[dev->num_vals++];
	v->type = (uint16_t)type;
	v->code = (uint16_t)code;
	v->value = value;
}

static void input_event_dispose(struct input_dev *dev, int disposition,
				unsigned int type, unsigned int code,
				int value)
{
	if ((disposition & INPUT_PASS_TO_DEVICE) && dev->event && dev->ready)
		dev->event(dev, type, code, value);

	if (disposition & INPUT_PASS_TO_HANDLERS) {
		if (disposition & INPUT_SLOT)
			input_append(dev, EV_ABS, ABS_MT_SLOT, dev->mt->slot);
		input_append(dev, type, code, value);
	}

	if (disposition & INPUT_FLUSH) {
		/* A packet of only its SYN_REPORT says nothing. */
		if (dev->num_vals >= 2)
			input_pass_values(dev);
		dev->num_vals = 0;
		/* The next packet gets a time of its own. */
		dev->timestamp_set = 0;
	} else if (dev->vals && dev->num_vals >= dev->max_vals - 2) {
		/* Out of room before the driver's SYN_REPORT: end the packet
		 * here (with value 1, telling it apart from a driver's own),
		 * so nothing is lost. */
		input_append(dev, EV_SYN, SYN_REPORT, 1);
		input_pass_values(dev);
		dev->num_vals = 0;
	}
}

static void input_handle_event(struct input_dev *dev, unsigned int type,
			       unsigned int code, int value)
{
	int disposition = input_get_disposition(dev, type, code, &value);

	if (disposition != INPUT_IGNORE_EVENT)
		input_event_dispose(dev, disposition, type, code, value);
}

/* EV_SYN is every device's, registered or not; any other type only when
 * the device declared it. */
static int input_type_supported(const struct input_dev *dev,
				unsigned int type)
{
	return type == EV_SYN || input_code_supported(type, dev->evbit, EV_MAX);
}

void input_event(struct input_dev *dev, unsigned int type, unsigned int code,
		 int value)
{
	uint64_t f;

	if (!dev || !input_type_supported(dev, type))
		return;
	spin_lock_irqsave(&dev->event_lock, &f);
	input_handle_event(dev, type, code, value);
	spin_unlock_irqrestore(&dev->event_lock, f);
}

void input_inject_event(struct input_dev *dev, unsigned int type,
			unsigned int code, int value)
{
	/* The event node is the only client that injects, and a grab is
	 * always held through it: a client's own writes are processed
	 * exactly like a driver's reports, grab or not. */
	input_event(dev, type, code, value);
}

void input_set_timestamp(struct input_dev *dev, uint64_t mono_ns)
{
	uint64_t f;

	if (!dev)
		return;
	spin_lock_irqsave(&dev->event_lock, &f);
	dev->timestamp_ns = mono_ns;
	dev->timestamp_set = 1;
	spin_unlock_irqrestore(&dev->event_lock, f);
}

/* Called from the multitouch helpers that already hold the event lock. */
void input_handle_event_locked(struct input_dev *dev, unsigned int type,
			       unsigned int code, int value)
{
	if (!input_type_supported(dev, type))
		return;
	input_handle_event(dev, type, code, value);
}

/* The software autorepeat: the key is still down, so it repeats, and the
 * next repeat is due a period later.  Process context (work item). */
static void input_repeat_key(struct work_struct *work)
{
	struct input_dev *dev =
		container_of(to_delayed_work(work), struct input_dev,
			     repeat_work);
	uint64_t f;

	spin_lock_irqsave(&dev->event_lock, &f);
	if (dev->repeat_live && input_test_bit(dev->repeat_key, dev->key) &&
	    input_code_supported(dev->repeat_key, dev->keybit, KEY_MAX)) {
		dev->timestamp_ns = timer_get_precise_ns();
		dev->timestamp_set = 1;
		input_handle_event(dev, EV_KEY, dev->repeat_key, 2);
		input_handle_event(dev, EV_SYN, SYN_REPORT, 1);
		if (dev->rep[REP_PERIOD] && system_wq)
			mod_delayed_work(system_wq, &dev->repeat_work,
					 msecs_to_jiffies((unsigned int)dev->rep[REP_PERIOD]));
	}
	spin_unlock_irqrestore(&dev->event_lock, f);
}

void input_enable_softrepeat(struct input_dev *dev, int delay, int period)
{
	if (!dev || WARN_ON(dev->static_storage))
		return;
	if (!dev->softrepeat) {
		/* Idle until the device is registered (repeat_live). */
		INIT_DELAYED_WORK(&dev->repeat_work, input_repeat_key);
		dev->softrepeat = 1;
	}
	dev->rep[REP_DELAY] = delay;
	dev->rep[REP_PERIOD] = period;
}

// ---------------------------------------------------------------------------
// Key code tables
// ---------------------------------------------------------------------------

int input_scancode_to_scalar(const struct input_keymap_entry *ke,
			     unsigned int *scancode)
{
	switch (ke->len) {
	case 1: {
		uint8_t v;

		kmemcpy(&v, ke->scancode, sizeof(v));
		*scancode = v;
		break;
	}
	case 2: {
		uint16_t v;

		kmemcpy(&v, ke->scancode, sizeof(v));
		*scancode = v;
		break;
	}
	case 4: {
		uint32_t v;

		kmemcpy(&v, ke->scancode, sizeof(v));
		*scancode = v;
		break;
	}
	default:
		return -EINVAL;
	}
	return 0;
}

static unsigned int input_fetch_keycode(const struct input_dev *dev,
					unsigned int index)
{
	switch (dev->keycodesize) {
	case 1:
		return ((const uint8_t *)dev->keycode)[index];
	case 2:
		return ((const uint16_t *)dev->keycode)[index];
	default:
		return ((const uint32_t *)dev->keycode)[index];
	}
}

int input_default_getkeycode(struct input_dev *dev,
			     struct input_keymap_entry *ke)
{
	unsigned int index;
	int error;

	if (!dev->keycodesize)
		return -EINVAL;
	if (ke->flags & INPUT_KEYMAP_BY_INDEX) {
		index = ke->index;
	} else {
		error = input_scancode_to_scalar(ke, &index);
		if (error)
			return error;
	}
	if (index >= dev->keycodemax)
		return -EINVAL;
	ke->keycode = input_fetch_keycode(dev, index);
	ke->index = (uint16_t)index;
	ke->len = sizeof(index);
	kmemcpy(ke->scancode, &index, sizeof(index));
	return 0;
}

int input_default_setkeycode(struct input_dev *dev,
			     const struct input_keymap_entry *ke,
			     unsigned int *old_keycode)
{
	unsigned int index, i;
	int error;

	if (!dev->keycodesize)
		return -EINVAL;
	if (ke->flags & INPUT_KEYMAP_BY_INDEX) {
		index = ke->index;
	} else {
		error = input_scancode_to_scalar(ke, &index);
		if (error)
			return error;
	}
	if (index >= dev->keycodemax)
		return -EINVAL;
	if (dev->keycodesize < sizeof(ke->keycode) &&
	    (ke->keycode >> (dev->keycodesize * 8)))
		return -EINVAL;

	switch (dev->keycodesize) {
	case 1: {
		uint8_t *k = (uint8_t *)dev->keycode;

		*old_keycode = k[index];
		k[index] = (uint8_t)ke->keycode;
		break;
	}
	case 2: {
		uint16_t *k = (uint16_t *)dev->keycode;

		*old_keycode = k[index];
		k[index] = (uint16_t)ke->keycode;
		break;
	}
	default: {
		uint32_t *k = (uint32_t *)dev->keycode;

		*old_keycode = k[index];
		k[index] = ke->keycode;
		break;
	}
	}

	/* The old key code stays declared only while some other entry still
	 * produces it. */
	if (*old_keycode <= KEY_MAX) {
		input_clear_bit(*old_keycode, dev->keybit);
		for (i = 0; i < dev->keycodemax; i++) {
			if (input_fetch_keycode(dev, i) == *old_keycode) {
				input_set_bit(*old_keycode, dev->keybit);
				break;
			}
		}
	}
	input_set_bit(ke->keycode, dev->keybit);
	return 0;
}

int input_get_keycode(struct input_dev *dev, struct input_keymap_entry *ke)
{
	uint64_t f;
	int rc;

	if (!dev->getkeycode)
		return -EINVAL;
	spin_lock_irqsave(&dev->event_lock, &f);
	rc = dev->getkeycode(dev, ke);
	spin_unlock_irqrestore(&dev->event_lock, f);
	return rc;
}

int input_set_keycode(struct input_dev *dev,
		      const struct input_keymap_entry *ke)
{
	unsigned int old_keycode;
	uint64_t f;
	int error;

	if (ke->keycode > KEY_MAX)
		return -EINVAL;
	if (!dev->setkeycode)
		return -EINVAL;
	spin_lock_irqsave(&dev->event_lock, &f);
	error = dev->setkeycode(dev, ke, &old_keycode);
	if (error)
		goto out;
	/* KEY_RESERVED is never reported. */
	input_clear_bit(KEY_RESERVED, dev->keybit);
	/* A key that is down and no longer in the table is released: the
	 * filter would drop the release as an undeclared key, so the event
	 * goes straight into the packet. */
	if (old_keycode > KEY_MAX) {
		kprintf("input: %s: got too big old keycode %#x\n",
			dev->name ? dev->name : "Unspecified device",
			old_keycode);
	} else if (input_test_bit(EV_KEY, dev->evbit) &&
		   !input_code_supported(old_keycode, dev->keybit, KEY_MAX) &&
		   input_test_bit(old_keycode, dev->key)) {
		input_clear_bit(old_keycode, dev->key);
		input_event_dispose(dev, INPUT_PASS_TO_HANDLERS, EV_KEY,
				    old_keycode, 0);
		input_event_dispose(dev, INPUT_PASS_TO_HANDLERS | INPUT_FLUSH,
				    EV_SYN, SYN_REPORT, 1);
	}
out:
	spin_unlock_irqrestore(&dev->event_lock, f);
	return error;
}

// ---------------------------------------------------------------------------
// Users
// ---------------------------------------------------------------------------

/* Push the device's logical LED / sound / autorepeat state down to it
 * (activate), or switch its LEDs and sounds off (!activate): at the first
 * open the hardware may have missed changes made while nobody had the
 * device open, and nothing should stay lit while nobody does.  event_lock
 * held; registered devices only. */
static void input_dev_toggle(struct input_dev *dev, int activate)
{
	unsigned int i;

	if (!dev->event || !dev->ready)
		return;
	if (input_test_bit(EV_LED, dev->evbit)) {
		for (i = 0; i < LED_CNT; i++) {
			int active;

			if (!input_test_bit(i, dev->ledbit))
				continue;
			active = input_test_bit(i, dev->led);
			if (!active && !activate)
				continue;
			dev->event(dev, EV_LED, i, activate ? active : 0);
		}
	}
	if (input_test_bit(EV_SND, dev->evbit)) {
		for (i = 0; i < SND_CNT; i++) {
			int active;

			if (!input_test_bit(i, dev->sndbit))
				continue;
			active = input_test_bit(i, dev->snd);
			if (!active && !activate)
				continue;
			dev->event(dev, EV_SND, i, activate ? active : 0);
		}
	}
	if (activate && input_test_bit(EV_REP, dev->evbit)) {
		dev->event(dev, EV_REP, REP_PERIOD, dev->rep[REP_PERIOD]);
		dev->event(dev, EV_REP, REP_DELAY, dev->rep[REP_DELAY]);
	}
}

/* One more user of either kind.  The driver's open() runs for the first
 * one; if it fails the count is not taken.  From then on the device takes
 * events of its own (event()), starting with its current state.
 * dev->mutex held. */
static int input_get_user_locked(struct input_dev *dev, unsigned int *count)
{
	uint64_t f;

	mutex_assert_held(&dev->mutex);
	if (dev->users + dev->kernel_users == 0) {
		if (dev->open) {
			int rc = dev->open(dev);

			if (rc)
				return rc;
		}
		if (!dev->static_storage) {
			spin_lock_irqsave(&dev->event_lock, &f);
			dev->ready = 1;
			input_dev_toggle(dev, 1);
			spin_unlock_irqrestore(&dev->event_lock, f);
		}
	}
	__atomic_store_n(count, *count + 1, __ATOMIC_RELAXED);
	return 0;
}

/* One user fewer; after the last one the device's LEDs and sounds go off,
 * it takes no events of its own any more, and the driver's close() runs.
 * dev->mutex held. */
static void input_put_user_locked(struct input_dev *dev, unsigned int *count)
{
	uint64_t f;

	mutex_assert_held(&dev->mutex);
	if (WARN_ON(*count == 0))
		return;
	__atomic_store_n(count, *count - 1, __ATOMIC_RELAXED);
	if (dev->users + dev->kernel_users == 0) {
		if (!dev->static_storage) {
			spin_lock_irqsave(&dev->event_lock, &f);
			input_dev_toggle(dev, 0);
			dev->ready = 0;
			spin_unlock_irqrestore(&dev->event_lock, f);
		}
		if (dev->close)
			dev->close(dev);
	}
}

/* Tell the handles that user space came or went.  dev->mutex held, so the
 * handle list cannot change underneath. */
static void input_notify_users_locked(struct input_dev *dev)
{
	struct input_handle *h;

	mutex_assert_held(&dev->mutex);
	for (h = dev->handles; h; h = h->next)
		if (h->users_changed)
			h->users_changed(h, dev->users);
}

int input_open_device(struct input_dev *dev)
{
	int rc;

	might_sleep();
	mutex_lock(&dev->mutex);
	if (dev->going_away) {
		rc = -ENODEV;
	} else {
		rc = input_get_user_locked(dev, &dev->users);
		if (!rc && dev->users == 1)
			input_notify_users_locked(dev);
	}
	mutex_unlock(&dev->mutex);
	return rc;
}

void input_close_device(struct input_dev *dev)
{
	unsigned int before;

	might_sleep();
	mutex_lock(&dev->mutex);
	before = dev->users;
	input_put_user_locked(dev, &dev->users);
	if (before == 1 && dev->users == 0)
		input_notify_users_locked(dev);
	mutex_unlock(&dev->mutex);
}

// ---------------------------------------------------------------------------
// In-kernel handles and handlers
// ---------------------------------------------------------------------------

int input_register_handle(struct input_dev *dev, struct input_handle *handle)
{
	struct input_handle *h;
	uint64_t f;
	int rc = 0;

	might_sleep();
	if (!dev || !handle || !handle->handler || dev->static_storage)
		return -EINVAL;
	mutex_lock(&dev->mutex);
	if (dev->going_away || !dev->registered) {
		rc = -ENODEV;
		goto out;
	}
	for (h = dev->handles; h; h = h->next) {
		if (WARN_ON(h == handle)) {
			rc = -EINVAL;
			goto out;
		}
	}
	handle->dev = dev;
	handle->open = 0;
	/* Linked under the event lock as well: the event path walks the
	 * list holding only that. */
	spin_lock_irqsave(&dev->event_lock, &f);
	handle->next = dev->handles;
	dev->handles = handle;
	spin_unlock_irqrestore(&dev->event_lock, f);
	/* The handle holds a reference until it is detached. */
	input_get_device(dev);
	/* A handle starts out assuming no user-space reader: tell it now
	 * when there is one (under the mutex, so no change is missed). */
	if (dev->users && handle->users_changed)
		handle->users_changed(handle, dev->users);
out:
	mutex_unlock(&dev->mutex);
	return rc;
}

void input_unregister_handle(struct input_handle *handle)
{
	struct input_handle **pp;
	struct input_dev *dev;
	uint64_t f;
	int found = 0;

	might_sleep();
	if (!handle || !handle->dev)
		return;
	dev = handle->dev;
	mutex_lock(&dev->mutex);
	while (handle->open > 0) {
		spin_lock_irqsave(&dev->event_lock, &f);
		handle->open--;
		spin_unlock_irqrestore(&dev->event_lock, f);
		input_put_user_locked(dev, &dev->kernel_users);
	}
	/* After the unlink no packet reaches the handle any more; one being
	 * delivered right now holds the event lock, so taking it here waits
	 * for that delivery to end. */
	spin_lock_irqsave(&dev->event_lock, &f);
	for (pp = &dev->handles; *pp; pp = &(*pp)->next) {
		if (*pp == handle) {
			*pp = handle->next;
			found = 1;
			break;
		}
	}
	spin_unlock_irqrestore(&dev->event_lock, f);
	mutex_unlock(&dev->mutex);
	WARN_ON(!found);
	handle->next = NULL;
	handle->dev = NULL;
	if (found)
		input_put_device(dev);
}

int input_handle_open(struct input_handle *handle)
{
	struct input_dev *dev;
	uint64_t f;
	int rc;

	might_sleep();
	if (!handle || !handle->dev)
		return -EINVAL;
	dev = handle->dev;
	mutex_lock(&dev->mutex);
	if (dev->going_away) {
		rc = -ENODEV;
	} else {
		rc = input_get_user_locked(dev, &dev->kernel_users);
		if (!rc) {
			/* Under the event lock: the event path reads it. */
			spin_lock_irqsave(&dev->event_lock, &f);
			handle->open++;
			spin_unlock_irqrestore(&dev->event_lock, f);
		}
	}
	mutex_unlock(&dev->mutex);
	return rc;
}

void input_handle_close(struct input_handle *handle)
{
	struct input_dev *dev;
	uint64_t f;

	might_sleep();
	if (!handle || !handle->dev)
		return;
	dev = handle->dev;
	mutex_lock(&dev->mutex);
	if (!WARN_ON(handle->open <= 0)) {
		spin_lock_irqsave(&dev->event_lock, &f);
		handle->open--;
		spin_unlock_irqrestore(&dev->event_lock, f);
		input_put_user_locked(dev, &dev->kernel_users);
	}
	mutex_unlock(&dev->mutex);
}

/* Offer a newly registered device to every handler.  Built-in devices
 * never get here. */
static void input_attach_handlers(struct input_dev *dev)
{
	struct input_handler *hd;

	might_sleep();
	mutex_lock(&g_input_handler_lock);
	for (hd = g_input_handlers; hd; hd = hd->next)
		if (hd->connect)
			hd->connect(hd, dev);
	dev->handlers_attached = 1;
	mutex_unlock(&g_input_handler_lock);
}

/* Ask the owners of the device's handles to detach them (all handles, or
 * only those of `only').  g_input_handler_lock held. */
static void input_detach_handles_locked(struct input_dev *dev,
					struct input_handler *only)
{
	for (;;) {
		struct input_handle *h, *x;

		mutex_assert_held(&g_input_handler_lock);
		mutex_lock(&dev->mutex);
		for (h = dev->handles; h; h = h->next)
			if (!only || h->handler == only)
				break;
		mutex_unlock(&dev->mutex);
		if (!h)
			return;
		if (h->handler && h->handler->disconnect)
			h->handler->disconnect(h);
		/* A disconnect that did not detach its handle would make this
		 * loop forever: detach it here.  Looked up by address only --
		 * a detached handle may be freed memory by now (and no new one
		 * can appear at that address: attaching needs the handler
		 * lock this thread holds). */
		mutex_lock(&dev->mutex);
		for (x = dev->handles; x; x = x->next)
			if (x == h)
				break;
		mutex_unlock(&dev->mutex);
		if (WARN_ON(x))
			input_unregister_handle(h);
	}
}

int input_register_handler(struct input_handler *handler)
{
	struct input_handler **pp;
	struct input_dev *dev, *prev = NULL;
	uint64_t f;

	might_sleep();
	if (!handler || !handler->connect)
		return -EINVAL;
	mutex_lock(&g_input_handler_lock);
	if (handler->registered) {
		mutex_unlock(&g_input_handler_lock);
		return -EBUSY;
	}
	handler->next = NULL;
	for (pp = &g_input_handlers; *pp; pp = &(*pp)->next)
		;
	*pp = handler;
	handler->registered = 1;

	/* The devices already there.  A device leaves the list only after
	 * its handles were detached, under this mutex, so `prev' stays on the
	 * list while it is held; devices are only appended. */
	for (;;) {
		spin_lock_irqsave(&g_input_list_lock, &f);
		dev = prev ? prev->next : g_input_head;
		while (dev && (dev->static_storage || !dev->handlers_attached))
			dev = dev->next;
		spin_unlock_irqrestore(&g_input_list_lock, f);
		if (!dev)
			break;
		handler->connect(handler, dev);
		prev = dev;
	}
	mutex_unlock(&g_input_handler_lock);
	return 0;
}

void input_unregister_handler(struct input_handler *handler)
{
	struct input_handler **pp;
	struct input_dev *dev, *prev = NULL;
	uint64_t f;

	might_sleep();
	if (!handler)
		return;
	mutex_lock(&g_input_handler_lock);
	if (!handler->registered) {
		mutex_unlock(&g_input_handler_lock);
		return;
	}
	/* As in input_register_handler(): only devices whose handles are
	 * attached.  One that is going away has already had them detached
	 * (handlers_attached cleared under this mutex) and may leave the
	 * list and be freed at any moment, so it must neither be detached
	 * again nor become `prev'.  A device with handles attached cannot
	 * leave the list while the mutex is held, so `prev' stays valid. */
	for (;;) {
		spin_lock_irqsave(&g_input_list_lock, &f);
		dev = prev ? prev->next : g_input_head;
		while (dev && (dev->static_storage || !dev->handlers_attached))
			dev = dev->next;
		spin_unlock_irqrestore(&g_input_list_lock, f);
		if (!dev)
			break;
		input_detach_handles_locked(dev, handler);
		prev = dev;
	}
	for (pp = &g_input_handlers; *pp; pp = &(*pp)->next) {
		if (*pp == handler) {
			*pp = handler->next;
			break;
		}
	}
	handler->next = NULL;
	handler->registered = 0;
	mutex_unlock(&g_input_handler_lock);
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

/* Events in a typical packet, from the capabilities: one per axis (one per
 * slot for the multitouch ones), one per relative axis, room for a few
 * keys and misc events, and the frame end. */
static unsigned int input_estimate_events_per_packet(const struct input_dev *dev)
{
	unsigned int slots, events, i;

	if (dev->mt) {
		slots = (unsigned int)dev->mt->num_slots;
	} else if (input_test_bit(ABS_MT_TRACKING_ID, dev->absbit)) {
		const struct input_absinfo *t =
			&dev->absinfo[ABS_MT_TRACKING_ID];
		int64_t span = (int64_t)t->maximum - t->minimum + 1;

		slots = span < 2 ? 2 : span > 32 ? 32 : (unsigned int)span;
	} else if (input_test_bit(ABS_MT_POSITION_X, dev->absbit)) {
		slots = 2;
	} else {
		slots = 0;
	}

	events = slots + 1; // SYN_MT_REPORT per contact + SYN_REPORT
	if (input_test_bit(EV_ABS, dev->evbit))
		for (i = 0; i < ABS_CNT; i++)
			if (input_test_bit(i, dev->absbit))
				events += input_is_mt_axis((int)i) ? slots : 1;
	if (input_test_bit(EV_REL, dev->evbit))
		for (i = 0; i < REL_CNT; i++)
			if (input_test_bit(i, dev->relbit))
				events++;
	return events + 7; // keys and misc events
}

/* A code bitmap whose type the device does not have must be empty: the
 * event filter only looks at the code bitmaps. */
static void input_cleanse_bitmasks(struct input_dev *dev)
{
#define CLEANSE(type, map)                                         \
	do {                                                       \
		if (!input_test_bit(type, dev->evbit))             \
			kmemset(dev->map, 0, sizeof(dev->map));    \
	} while (0)
	CLEANSE(EV_KEY, keybit);
	CLEANSE(EV_REL, relbit);
	CLEANSE(EV_ABS, absbit);
	CLEANSE(EV_MSC, mscbit);
	CLEANSE(EV_LED, ledbit);
	CLEANSE(EV_SND, sndbit);
	CLEANSE(EV_FF, ffbit);
	CLEANSE(EV_SW, swbit);
#undef CLEANSE
}

static void input_prepare(struct input_dev *dev)
{
	unsigned int est;

	input_set_bit(EV_SYN, dev->evbit);
	/* Code 0 of EV_KEY means "no key"; it is never reported. */
	input_clear_bit(KEY_RESERVED, dev->keybit);
	input_cleanse_bitmasks(dev);
	est = input_estimate_events_per_packet(dev);
	if (dev->hint_events_per_packet < est)
		dev->hint_events_per_packet = est;
	if (!dev->getkeycode)
		dev->getkeycode = input_default_getkeycode;
	if (!dev->setkeycode)
		dev->setkeycode = input_default_setkeycode;
}

/* Put the device on the list with the lowest free inputN number.
 * 0 or -ENFILE. */
static int input_list_add(struct input_dev *dev)
{
	struct input_dev **pp;
	uint64_t f;
	int n;

	spin_lock_irqsave(&g_input_list_lock, &f);
	for (n = 0; n < INPUT_MAX_NUMBERS; n++)
		if (!input_test_bit((unsigned int)n, g_input_numbers))
			break;
	if (n == INPUT_MAX_NUMBERS) {
		spin_unlock_irqrestore(&g_input_list_lock, f);
		return -ENFILE;
	}
	input_set_bit((unsigned int)n, g_input_numbers);
	dev->input_no = n;
	dev->next = NULL;
	for (pp = &g_input_head; *pp; pp = &(*pp)->next)
		;
	*pp = dev;
	dev->registered = 1;
	spin_unlock_irqrestore(&g_input_list_lock, f);
	return 0;
}

static void input_list_del(struct input_dev *dev)
{
	struct input_dev **pp;
	uint64_t f;

	spin_lock_irqsave(&g_input_list_lock, &f);
	for (pp = &g_input_head; *pp; pp = &(*pp)->next) {
		if (*pp == dev) {
			*pp = dev->next;
			break;
		}
	}
	if (dev->input_no >= 0 && dev->input_no < INPUT_MAX_NUMBERS)
		input_clear_bit((unsigned int)dev->input_no, g_input_numbers);
	dev->input_no = -1;
	dev->next = NULL;
	dev->registered = 0;
	spin_unlock_irqrestore(&g_input_list_lock, f);
}

static void input_publish(struct input_dev *dev, uint32_t type, int minor)
{
	char name[16];
	uint32_t flags = input_classify(dev);

	if (minor < 0)
		return;
	if (dev->id.bustype == BUS_USB)
		flags |= DEVICE_EVENT_F_USB;
	ksnprintf(name, sizeof(name), "event%d", minor);
	devevent_publish(type, DEVICE_CLASS_INPUT,
			 ((uint64_t)dev->id.bustype << 48) |
				 ((uint64_t)dev->id.version << 32) |
				 (uint64_t)(uint32_t)minor,
			 0, flags, dev->id.vendor, dev->id.product,
			 EVDEV_MAJOR, EVDEV_MINOR_BASE + (uint32_t)minor, name);
}

int input_register_device(struct input_dev *dev)
{
	struct input_value *vals;
	unsigned int max_vals;
	uint64_t f;
	int rc, minor;

	might_sleep();
	if (!dev || dev->registered || dev->static_storage || dev->going_away)
		return -EINVAL;
	/* The built-in keyboard and mouse are always event0/event1 and
	 * input0/input1, whatever registers first. */
	evdev_builtin_init();

	input_prepare(dev);
	/* Timings preset by the driver mean it repeats keys itself. */
	if (!dev->rep[REP_DELAY] && !dev->rep[REP_PERIOD])
		input_enable_softrepeat(dev, 250, 33);
	max_vals = dev->hint_events_per_packet + 2;
	vals = (struct input_value *)kcalloc(max_vals, sizeof(*vals));
	if (!vals)
		return -ENOMEM;
	spin_lock_irqsave(&dev->event_lock, &f);
	kfree(dev->vals); // a previous, failed registration's
	dev->vals = vals;
	dev->max_vals = max_vals;
	dev->num_vals = 0;
	dev->timestamp_set = 0;
	spin_unlock_irqrestore(&dev->event_lock, f);

	rc = input_list_add(dev);
	if (rc)
		return rc;
	rc = evdev_connect(dev);
	if (rc) {
		input_list_del(dev);
		return rc;
	}
	minor = evdev_minor_of(dev->evdev);
	kprintf("input: %s as /dev/input/event%d (input%d)\n",
		dev->name ? dev->name : "Unspecified device", minor,
		dev->input_no);
	input_publish(dev, DEVICE_EVENT_ADD, minor);
	spin_lock_irqsave(&dev->event_lock, &f);
	dev->repeat_live = 1;
	spin_unlock_irqrestore(&dev->event_lock, f);
	input_attach_handlers(dev);
	return 0;
}

int input_register_builtin_device(struct input_dev *dev, struct evdev *node,
				  int minor)
{
	int rc;

	if (!dev || !node || !dev->vals || dev->max_vals < 3)
		return -EINVAL;
	input_dev_init_common(dev);
	dev->static_storage = 1;
	/* The keyboard driver's LED path: events of its own at any time. */
	dev->ready = 1;
	input_prepare(dev);
	rc = input_list_add(dev);
	if (rc)
		return rc;
	rc = evdev_connect_builtin(dev, node, minor);
	if (rc) {
		input_list_del(dev);
		return rc;
	}
	input_publish(dev, DEVICE_EVENT_ADD, minor);
	return 0;
}

void input_unregister_device(struct input_dev *dev)
{
	unsigned int w, b;
	uint64_t f;
	int minor;

	might_sleep();
	if (!dev || WARN_ON(!dev->registered) || WARN_ON(dev->static_storage))
		return;

	/* No new users from here on. */
	mutex_lock(&dev->mutex);
	dev->going_away = 1;
	mutex_unlock(&dev->mutex);

	/* Readers must not be left with keys that stay down for ever: every
	 * key still down is released, in one last packet. */
	spin_lock_irqsave(&dev->event_lock, &f);
	if (input_test_bit(EV_KEY, dev->evbit)) {
		int need_sync = 0;

		for (w = 0; w < INPUT_BITS_TO_LONGS(KEY_CNT); w++) {
			unsigned long down = dev->key[w];

			for (b = 0; down && b < INPUT_BITS_PER_LONG; b++) {
				if (!(down & (1UL << b)))
					continue;
				down &= ~(1UL << b);
				input_handle_event(dev, EV_KEY,
						   w * INPUT_BITS_PER_LONG + b,
						   0);
				need_sync = 1;
			}
		}
		if (need_sync)
			input_handle_event(dev, EV_SYN, SYN_REPORT, 1);
	}
	/* No more autorepeat (the releases above stopped it already, a
	 * driver reporting on would arm it again). */
	dev->repeat_live = 0;
	minor = dev->evdev ? evdev_minor_of(dev->evdev) : -1;
	spin_unlock_irqrestore(&dev->event_lock, f);
	if (dev->softrepeat)
		cancel_delayed_work_sync(&dev->repeat_work);

	/* The in-kernel listeners let go (they saw the key releases). */
	mutex_lock(&g_input_handler_lock);
	input_detach_handles_locked(dev, NULL);
	dev->handlers_attached = 0;
	mutex_unlock(&g_input_handler_lock);

	/* The node goes: readers get ENODEV, the device is closed if it was
	 * open, no more events reach anybody. */
	evdev_disconnect(dev);
	input_list_del(dev);
	input_publish(dev, DEVICE_EVENT_REMOVE, minor);
	kprintf("input: %s removed (event%d)\n",
		dev->name ? dev->name : "Unspecified device", minor);
	/* Drop what input_allocate_device() handed the driver. */
	input_put_device(dev);
}

// ---------------------------------------------------------------------------
// Classification
// ---------------------------------------------------------------------------

static int input_any_key_in(const struct input_dev *dev, unsigned int from,
			    unsigned int to)
{
	for (unsigned int c = from; c <= to && c <= KEY_MAX; c++)
		if (input_test_bit(c, dev->keybit))
			return 1;
	return 0;
}

uint32_t input_classify(const struct input_dev *dev)
{
	uint32_t f = 0;
	int has_key = input_test_bit(EV_KEY, dev->evbit);
	int has_abs = input_test_bit(EV_ABS, dev->evbit);
	int has_rel = input_test_bit(EV_REL, dev->evbit);
	int abs_xy = has_abs && input_test_bit(ABS_X, dev->absbit) &&
		     input_test_bit(ABS_Y, dev->absbit);
	int mt_xy = has_abs &&
		    input_test_bit(ABS_MT_POSITION_X, dev->absbit) &&
		    input_test_bit(ABS_MT_POSITION_Y, dev->absbit);
	int rel_xy = has_rel && input_test_bit(REL_X, dev->relbit) &&
		     input_test_bit(REL_Y, dev->relbit);
	int direct = input_test_bit(INPUT_PROP_DIRECT, dev->propbit);
	int touch = has_key && input_test_bit(BTN_TOUCH, dev->keybit);
	int finger = has_key && input_test_bit(BTN_TOOL_FINGER, dev->keybit);
	int pen = has_key && (input_test_bit(BTN_TOOL_PEN, dev->keybit) ||
			      input_test_bit(BTN_STYLUS, dev->keybit));
	int mouse_btn = has_key && input_any_key_in(dev, BTN_MOUSE, BTN_TASK);
	int joy_btn = has_key &&
		      (input_any_key_in(dev, BTN_JOYSTICK, BTN_JOYSTICK + 0xf) ||
		       input_any_key_in(dev, BTN_GAMEPAD, BTN_THUMBR) ||
		       input_any_key_in(dev, BTN_TRIGGER_HAPPY,
					BTN_TRIGGER_HAPPY40));

	if (input_test_bit(INPUT_PROP_ACCELEROMETER, dev->propbit))
		return DEVICE_EVENT_F_INPUT_ACCELEROMETER;

	if (abs_xy || mt_xy) {
		if (pen)
			f |= DEVICE_EVENT_F_INPUT_TABLET;
		else if (finger && !direct)
			f |= DEVICE_EVENT_F_INPUT_TOUCHPAD;
		else if (touch || direct)
			f |= DEVICE_EVENT_F_INPUT_TOUCHSCREEN;
		else if (mouse_btn)
			f |= DEVICE_EVENT_F_INPUT_MOUSE; // absolute pointer
		else if (joy_btn || !has_key)
			f |= DEVICE_EVENT_F_INPUT_JOYSTICK;
	} else if (joy_btn && !mouse_btn) {
		f |= DEVICE_EVENT_F_INPUT_JOYSTICK;
	}
	if (rel_xy && mouse_btn)
		f |= DEVICE_EVENT_F_INPUT_MOUSE;
	if (input_test_bit(INPUT_PROP_POINTING_STICK, dev->propbit))
		f |= DEVICE_EVENT_F_INPUT_POINTINGSTICK;

	if (has_key) {
		/* A keyboard has the whole first row of ordinary keys:
		 * Esc, the digits, the letters of the top rows (codes 1..31,
		 * KEY_ESC..KEY_S).
		 * Anything with some ordinary keys is at least "keys". */
		int all = 1;

		for (unsigned int c = KEY_ESC; c <= KEY_S; c++)
			if (!input_test_bit(c, dev->keybit))
				all = 0;
		if (all)
			f |= DEVICE_EVENT_F_INPUT_KEYBOARD;
		if (input_any_key_in(dev, KEY_ESC, BTN_MISC - 1) ||
		    input_any_key_in(dev, KEY_OK, BTN_DPAD_UP - 1) ||
		    input_any_key_in(dev, KEY_ALS_TOGGLE, BTN_TRIGGER_HAPPY - 1))
			f |= DEVICE_EVENT_F_INPUT_KEY;
	}
	if (input_test_bit(EV_SW, dev->evbit))
		f |= DEVICE_EVENT_F_INPUT_SWITCH;
	return f;
}

// ---------------------------------------------------------------------------
// /proc/bus/input/devices
// ---------------------------------------------------------------------------

static long put_char(char *buf, long cap, long pos, char c)
{
	if (pos >= 0 && pos < cap)
		buf[pos] = c;
	return pos + 1;
}

static long put_str(char *buf, long cap, long pos, const char *s)
{
	while (s && *s)
		pos = put_char(buf, cap, pos, *s++);
	return pos;
}

/* Formatted pieces without strings of unknown length (the identity line,
 * numbers): the scratch buffer holds 128 characters, and a longer result
 * would be cut. */
static long put_fmt(char *buf, long cap, long pos, const char *fmt, ...)
{
	char tmp[128];
	__builtin_va_list ap;
	int n;

	__builtin_va_start(ap, fmt);
	n = kvsnprintf(tmp, sizeof(tmp), fmt, ap);
	__builtin_va_end(ap);
	if (n < 0)
		return pos;
	if (n >= (int)sizeof(tmp))
		n = (int)sizeof(tmp) - 1;
	for (int i = 0; i < n; i++)
		pos = put_char(buf, cap, pos, tmp[i]);
	return pos;
}

/* "B: NAME=w_hi ... w_0": the words of the bitmap from the highest one that
 * is not zero down to word 0, in hex, separated by a space; "0" for an
 * empty bitmap.  `max' is the type's highest code. */
static long put_bitmap(char *buf, long cap, long pos, const char *name,
		       const unsigned long *map, unsigned int max)
{
	int i = (int)INPUT_BITS_TO_LONGS(max) - 1;

	while (i > 0 && !map[i])
		i--;
	pos = put_str(buf, cap, pos, "B: ");
	pos = put_str(buf, cap, pos, name);
	pos = put_char(buf, cap, pos, '=');
	for (; i >= 0; i--) {
		pos = put_fmt(buf, cap, pos, "%lx", map[i]);
		if (i > 0)
			pos = put_char(buf, cap, pos, ' ');
	}
	return put_char(buf, cap, pos, '\n');
}

long input_format_device(const struct input_dev *dev, int evdev_minor,
			 char *buf, long cap, long pos)
{
	pos = put_fmt(buf, cap, pos,
		      "I: Bus=%04x Vendor=%04x Product=%04x Version=%04x\n",
		      dev->id.bustype, dev->id.vendor, dev->id.product,
		      dev->id.version);
	pos = put_str(buf, cap, pos, "N: Name=\"");
	pos = put_str(buf, cap, pos, dev->name);
	pos = put_str(buf, cap, pos, "\"\nP: Phys=");
	pos = put_str(buf, cap, pos, dev->phys);
	pos = put_str(buf, cap, pos, "\nS: Sysfs=");
	pos = put_str(buf, cap, pos,
		      dev->devpath ? dev->devpath : "/devices/virtual");
	pos = put_fmt(buf, cap, pos, "/input/input%d\n", dev->input_no);
	pos = put_str(buf, cap, pos, "U: Uniq=");
	pos = put_str(buf, cap, pos, dev->uniq);
	pos = put_str(buf, cap, pos, "\nH: Handlers=");
	if (evdev_minor >= 0)
		pos = put_fmt(buf, cap, pos, "event%d ", evdev_minor);
	pos = put_char(buf, cap, pos, '\n');

	pos = put_bitmap(buf, cap, pos, "PROP", dev->propbit, INPUT_PROP_MAX);
	pos = put_bitmap(buf, cap, pos, "EV", dev->evbit, EV_MAX);
	if (input_test_bit(EV_KEY, dev->evbit))
		pos = put_bitmap(buf, cap, pos, "KEY", dev->keybit, KEY_MAX);
	if (input_test_bit(EV_REL, dev->evbit))
		pos = put_bitmap(buf, cap, pos, "REL", dev->relbit, REL_MAX);
	if (input_test_bit(EV_ABS, dev->evbit))
		pos = put_bitmap(buf, cap, pos, "ABS", dev->absbit, ABS_MAX);
	if (input_test_bit(EV_MSC, dev->evbit))
		pos = put_bitmap(buf, cap, pos, "MSC", dev->mscbit, MSC_MAX);
	if (input_test_bit(EV_LED, dev->evbit))
		pos = put_bitmap(buf, cap, pos, "LED", dev->ledbit, LED_MAX);
	if (input_test_bit(EV_SND, dev->evbit))
		pos = put_bitmap(buf, cap, pos, "SND", dev->sndbit, SND_MAX);
	if (input_test_bit(EV_FF, dev->evbit))
		pos = put_bitmap(buf, cap, pos, "FF", dev->ffbit, FF_MAX);
	if (input_test_bit(EV_SW, dev->evbit))
		pos = put_bitmap(buf, cap, pos, "SW", dev->swbit, SW_MAX);
	return put_char(buf, cap, pos, '\n');
}

long input_proc_devices_show(struct pfs_node *n, char *buf, long cap)
{
	struct input_dev *dev;
	uint64_t f, ef;
	long pos = 0;

	(void)n;
	evdev_builtin_init();
	spin_lock_irqsave(&g_input_list_lock, &f);
	for (dev = g_input_head; dev; dev = dev->next) {
		int minor;

		/* The node link changes under the event lock only, and the
		 * node is not freed before it has been unlinked there. */
		spin_lock_irqsave(&dev->event_lock, &ef);
		minor = dev->evdev ? evdev_minor_of(dev->evdev) : -1;
		spin_unlock_irqrestore(&dev->event_lock, ef);
		pos = input_format_device(dev, minor, buf, cap, pos);
	}
	spin_unlock_irqrestore(&g_input_list_lock, f);
	return pos;
}
