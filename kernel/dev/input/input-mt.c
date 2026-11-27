// LikeOS input core - multitouch slots
//
// Slot bookkeeping on top of the input core: tracking-id allocation, the
// frame counter that tells which slots a driver reported in the current
// hardware frame, release of the ones it did not, the single-touch pointer
// emulation (BTN_TOUCH, ABS_X/ABS_Y from the oldest contact, the BTN_TOOL_*
// finger count) and, for devices whose contacts have no identity of their
// own, matching contacts to slots by position.
//
// Every reporting helper here runs a whole step under the device's event
// spinlock, so it is safe from any context and a frame is never torn by a
// concurrent EVIOCGMTSLOTS snapshot.  The values a helper reads back (a
// slot's tracking id, its position) are the core's cached values, which the
// event filter in input.c keeps current as events pass.
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) 2008-2010 Henrik Rydberg

#include <kernel/dev/input/input-mt.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h> /* errno values */

// ---------------------------------------------------------------------------
// Set-up
// ---------------------------------------------------------------------------

/* The single-touch view of an MT axis: same range, no fuzz of its own (the
 * noise was filtered on the source). */
static void copy_abs(struct input_dev *dev, unsigned int dst, unsigned int src)
{
	if (input_test_bit(src, dev->absbit)) {
		dev->absinfo[dst] = dev->absinfo[src];
		dev->absinfo[dst].fuzz = 0;
		input_set_bit(dst, dev->absbit);
	}
}

int input_mt_init_slots(struct input_dev *dev, unsigned int num_slots,
			unsigned int flags)
{
	struct input_mt *mt;
	unsigned int i;

	might_sleep();
	if (!dev)
		return -EINVAL;
	if (!num_slots)
		return 0;
	if (dev->mt)
		return dev->mt->num_slots != (int)num_slots ? -EINVAL : 0;
	/* Arbitrary limit for avoiding too large memory allocation. */
	if (num_slots > 1024)
		return -EINVAL;

	mt = (struct input_mt *)kcalloc(1, sizeof(*mt) + num_slots *
						       sizeof(struct input_mt_slot));
	if (!mt)
		return -ENOMEM;
	mt->num_slots = (int)num_slots;
	mt->flags = flags;
	if (flags & INPUT_MT_TRACK) {
		/* Work space allocated up front: assigning slots at interrupt
		 * time never allocates. */
		mt->red = (int *)kcalloc((size_t)num_slots * num_slots,
					 sizeof(int));
		if (!mt->red) {
			kfree(mt);
			return -ENOMEM;
		}
	}

	input_set_abs_params(dev, ABS_MT_SLOT, 0, (int)num_slots - 1, 0, 0);
	input_set_abs_params(dev, ABS_MT_TRACKING_ID, 0, INPUT_MT_TRKID_MAX, 0,
			     0);

	if (flags & (INPUT_MT_POINTER | INPUT_MT_DIRECT)) {
		/* The single-touch view of the device, fed by the pointer
		 * emulation: BTN_TOUCH and the oldest contact's position. */
		input_set_bit(EV_KEY, dev->evbit);
		input_set_bit(BTN_TOUCH, dev->keybit);
		copy_abs(dev, ABS_X, ABS_MT_POSITION_X);
		copy_abs(dev, ABS_Y, ABS_MT_POSITION_Y);
		copy_abs(dev, ABS_PRESSURE, ABS_MT_PRESSURE);
	}
	if (flags & INPUT_MT_POINTER) {
		/* The finger count, as far as the slots can see. */
		input_set_bit(BTN_TOOL_FINGER, dev->keybit);
		input_set_bit(BTN_TOOL_DOUBLETAP, dev->keybit);
		if (num_slots >= 3)
			input_set_bit(BTN_TOOL_TRIPLETAP, dev->keybit);
		if (num_slots >= 4)
			input_set_bit(BTN_TOOL_QUADTAP, dev->keybit);
		if (num_slots >= 5)
			input_set_bit(BTN_TOOL_QUINTTAP, dev->keybit);
		input_set_bit(INPUT_PROP_POINTER, dev->propbit);
	}
	if (flags & INPUT_MT_DIRECT)
		input_set_bit(INPUT_PROP_DIRECT, dev->propbit);
	if (flags & INPUT_MT_SEMI_MT)
		input_set_bit(INPUT_PROP_SEMI_MT, dev->propbit);

	/* Every slot starts empty, and none counts as reported in the first
	 * frame (frame 1; the slots say 0). */
	for (i = 0; i < num_slots; i++)
		input_mt_set_value(&mt->slots[i], ABS_MT_TRACKING_ID, -1);
	mt->frame = 1;
	dev->mt = mt;
	return 0;
}

void input_mt_destroy_slots(struct input_dev *dev)
{
	if (!dev || !dev->mt)
		return;
	kfree(dev->mt->red);
	kfree(dev->mt);
	dev->mt = NULL;
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

static int input_mt_report_slot_state_locked(struct input_dev *dev,
					     unsigned int tool_type, int active)
{
	struct input_mt *mt = dev->mt;
	struct input_mt_slot *slot;
	int id;

	if (!mt)
		return 0;
	slot = &mt->slots[mt->slot];
	slot->frame = mt->frame; // reported in this frame
	if (!active) {
		input_handle_event_locked(dev, EV_ABS, ABS_MT_TRACKING_ID, -1);
		return 0;
	}
	/* A contact that was already in the slot keeps its id; a new one
	 * gets the next. */
	id = input_mt_get_value(slot, ABS_MT_TRACKING_ID);
	if (id < 0)
		id = input_mt_new_trkid(mt);
	input_handle_event_locked(dev, EV_ABS, ABS_MT_TRACKING_ID, id);
	input_handle_event_locked(dev, EV_ABS, ABS_MT_TOOL_TYPE,
				  (int)tool_type);
	return 1;
}

int input_mt_report_slot_state(struct input_dev *dev, unsigned int tool_type,
			       int active)
{
	uint64_t f;
	int rc;

	if (!dev || !dev->mt)
		return 0;
	spin_lock_irqsave(&dev->event_lock, &f);
	rc = input_mt_report_slot_state_locked(dev, tool_type, active);
	spin_unlock_irqrestore(&dev->event_lock, f);
	return rc;
}

static void input_mt_report_finger_count_locked(struct input_dev *dev,
						int count)
{
	input_handle_event_locked(dev, EV_KEY, BTN_TOOL_FINGER, count == 1);
	input_handle_event_locked(dev, EV_KEY, BTN_TOOL_DOUBLETAP, count == 2);
	input_handle_event_locked(dev, EV_KEY, BTN_TOOL_TRIPLETAP, count == 3);
	input_handle_event_locked(dev, EV_KEY, BTN_TOOL_QUADTAP, count == 4);
	input_handle_event_locked(dev, EV_KEY, BTN_TOOL_QUINTTAP, count == 5);
}

void input_mt_report_finger_count(struct input_dev *dev, int count)
{
	uint64_t f;

	if (!dev)
		return;
	spin_lock_irqsave(&dev->event_lock, &f);
	input_mt_report_finger_count_locked(dev, count);
	spin_unlock_irqrestore(&dev->event_lock, f);
}

static void input_mt_report_pointer_emulation_locked(struct input_dev *dev,
						     int use_count)
{
	struct input_mt *mt = dev->mt;
	struct input_mt_slot *oldest = NULL;
	int oldid, count = 0, i;
	int p, reported_p = 0;

	if (!mt)
		return;
	/* The oldest contact is the one whose tracking id is furthest behind
	 * the next id to be handed out, counting in the wrapping id space:
	 * id - oldid negative (sign bit of the id space set) means older. */
	oldid = mt->trkid;
	for (i = 0; i < mt->num_slots; i++) {
		struct input_mt_slot *ps = &mt->slots[i];
		int id = input_mt_get_value(ps, ABS_MT_TRACKING_ID);

		if (id < 0)
			continue;
		if (((unsigned int)id - (unsigned int)oldid) &
		    INPUT_MT_TRKID_SIGN) {
			oldest = ps;
			oldid = id;
		}
		if (input_test_bit(ABS_MT_PRESSURE, dev->absbit)) {
			p = input_mt_get_value(ps, ABS_MT_PRESSURE);
			if (mt->flags & INPUT_MT_TOTAL_FORCE)
				reported_p = (int)((unsigned int)reported_p +
						   (unsigned int)p);
			else if (oldid == id)
				reported_p = p;
		}
		count++;
	}

	input_handle_event_locked(dev, EV_KEY, BTN_TOUCH, count > 0);
	if (use_count) {
		/* A device that reports hovering only as a general distance
		 * (no per-contact ABS_MT_DISTANCE) still has a finger in
		 * proximity. */
		if (count == 0 &&
		    !input_test_bit(ABS_MT_DISTANCE, dev->absbit) &&
		    input_test_bit(ABS_DISTANCE, dev->absbit) &&
		    input_abs_get_val(dev, ABS_DISTANCE) != 0)
			count = 1;
		input_mt_report_finger_count_locked(dev, count);
	}

	if (oldest) {
		input_handle_event_locked(
			dev, EV_ABS, ABS_X,
			input_mt_get_value(oldest, ABS_MT_POSITION_X));
		input_handle_event_locked(
			dev, EV_ABS, ABS_Y,
			input_mt_get_value(oldest, ABS_MT_POSITION_Y));
		if (input_test_bit(ABS_MT_PRESSURE, dev->absbit))
			input_handle_event_locked(dev, EV_ABS, ABS_PRESSURE,
						  reported_p);
	} else if (input_test_bit(ABS_MT_PRESSURE, dev->absbit)) {
		input_handle_event_locked(dev, EV_ABS, ABS_PRESSURE, 0);
	}
}

void input_mt_report_pointer_emulation(struct input_dev *dev, int use_count)
{
	uint64_t f;

	if (!dev || !dev->mt)
		return;
	spin_lock_irqsave(&dev->event_lock, &f);
	input_mt_report_pointer_emulation_locked(dev, use_count);
	spin_unlock_irqrestore(&dev->event_lock, f);
}

/* Release the slots that hold a contact but were not reported in this
 * frame. */
static void input_mt_drop_unused_locked(struct input_dev *dev)
{
	struct input_mt *mt = dev->mt;
	int i;

	for (i = 0; i < mt->num_slots; i++) {
		struct input_mt_slot *ps = &mt->slots[i];

		if (input_mt_is_active(ps) && !input_mt_is_used(mt, ps)) {
			input_handle_event_locked(dev, EV_ABS, ABS_MT_SLOT, i);
			input_handle_event_locked(dev, EV_ABS,
						  ABS_MT_TRACKING_ID, -1);
		}
	}
}

void input_mt_drop_unused(struct input_dev *dev)
{
	uint64_t f;

	if (!dev || !dev->mt)
		return;
	spin_lock_irqsave(&dev->event_lock, &f);
	input_mt_drop_unused_locked(dev);
	dev->mt->frame++;
	spin_unlock_irqrestore(&dev->event_lock, f);
}

void input_mt_sync_frame(struct input_dev *dev)
{
	struct input_mt *mt;
	uint64_t f;

	if (!dev || !dev->mt)
		return;
	spin_lock_irqsave(&dev->event_lock, &f);
	mt = dev->mt;
	if (mt->flags & INPUT_MT_DROP_UNUSED)
		input_mt_drop_unused_locked(dev);
	/* A semi-mt device knows the bounding box, not the contacts: its
	 * finger count comes from the driver, not from the slots. */
	input_mt_report_pointer_emulation_locked(
		dev, (mt->flags & INPUT_MT_POINTER) &&
			     !(mt->flags & INPUT_MT_SEMI_MT));
	mt->frame++;
	spin_unlock_irqrestore(&dev->event_lock, f);
}

// ---------------------------------------------------------------------------
// Slot selection for drivers
// ---------------------------------------------------------------------------

int input_mt_get_slot_by_key(struct input_dev *dev, int key)
{
	struct input_mt *mt;
	uint64_t f;
	int i, slot = -1;

	if (!dev || !dev->mt)
		return -1;
	spin_lock_irqsave(&dev->event_lock, &f);
	mt = dev->mt;
	for (i = 0; i < mt->num_slots; i++) {
		struct input_mt_slot *s = &mt->slots[i];

		if (input_mt_is_active(s) && s->key == (unsigned int)key) {
			slot = i;
			goto out;
		}
	}
	/* A slot that is empty and was not given to anybody in this frame
	 * (one released in this frame still has its release to report). */
	for (i = 0; i < mt->num_slots; i++) {
		struct input_mt_slot *s = &mt->slots[i];

		if (!input_mt_is_active(s) && !input_mt_is_used(mt, s)) {
			s->key = (unsigned int)key;
			slot = i;
			goto out;
		}
	}
out:
	spin_unlock_irqrestore(&dev->event_lock, f);
	return slot;
}

/* The balanced best match of input_mt_assign_slots(): a reduced cost matrix
 * whose rows are the active slots and whose columns are the contacts, with
 * mu = 2 * dmax^2 subtracted from every squared distance.  The duals of the
 * rows and of the columns are adjusted in turn until every row and column
 * holds at most one negative entry; a negative entry is a match.  The
 * arithmetic wraps on overflow (done in unsigned), as the algorithm's
 * fixed-width integers always have. */
static int wrap_add(int a, int b)
{
	return (int)((unsigned int)a + (unsigned int)b);
}

static int wrap_sub(int a, int b)
{
	return (int)((unsigned int)a - (unsigned int)b);
}

static int adjust_dual(int *begin, int step, int *end, int eq, int mu)
{
	int f, *p, s, c;

	if (begin == end)
		return 0;

	f = *begin;
	p = begin + step;
	s = p == end ? wrap_add(f, 1) : *p;

	for (; p != end; p += step) {
		if (*p < f) {
			s = f;
			f = *p;
		} else if (*p < s) {
			s = *p;
		}
	}

	c = wrap_add(wrap_add(f, s), 1) / 2;
	if (c == 0 || (c > mu && (!eq || mu > 0)))
		return 0;
	/* Improve convergence for positive matrices by penalizing overcovers */
	if (s < 0 && mu <= 0)
		c = wrap_add(c, c);

	for (p = begin; p != end; p += step)
		*p = wrap_sub(*p, c);

	return (c < s && s <= 0) || (f >= 0 && f < c);
}

static void find_reduced_matrix(int *w, int nr, int nc, int nrc, int mu)
{
	int i, k, sum;

	for (k = 0; k < nrc; k++) {
		for (i = 0; i < nr; i++)
			adjust_dual(w + i, nr, w + i + nrc, nr <= nc, mu);
		sum = 0;
		for (i = 0; i < nrc; i += nr)
			sum += adjust_dual(w + i, 1, w + i + nr, nc <= nr, mu);
		if (!sum)
			break;
	}
}

static int input_mt_set_matrix(struct input_mt *mt,
			       const struct input_mt_pos *pos, int num_pos,
			       int mu)
{
	const struct input_mt_pos *p;
	struct input_mt_slot *s;
	int *w = mt->red;
	int x, y;

	for (s = mt->slots; s != mt->slots + mt->num_slots; s++) {
		if (!input_mt_is_active(s))
			continue;
		x = input_mt_get_value(s, ABS_MT_POSITION_X);
		y = input_mt_get_value(s, ABS_MT_POSITION_Y);
		for (p = pos; p != pos + num_pos; p++) {
			unsigned int dx = (unsigned int)x - (unsigned int)p->x;
			unsigned int dy = (unsigned int)y - (unsigned int)p->y;

			*w++ = (int)(dx * dx + dy * dy - (unsigned int)mu);
		}
	}

	return (int)(w - mt->red);
}

static void input_mt_set_slots(struct input_mt *mt, int *slots, int num_pos)
{
	struct input_mt_slot *s;
	int *w = mt->red, j;

	for (j = 0; j != num_pos; j++)
		slots[j] = -1;

	for (s = mt->slots; s != mt->slots + mt->num_slots; s++) {
		if (!input_mt_is_active(s))
			continue;

		for (j = 0; j != num_pos; j++) {
			if (w[j] < 0) {
				slots[j] = (int)(s - mt->slots);
				break;
			}
		}

		w += num_pos;
	}

	/* New contacts go to empty slots, lowest first. */
	for (s = mt->slots; s != mt->slots + mt->num_slots; s++) {
		if (input_mt_is_active(s))
			continue;

		for (j = 0; j != num_pos; j++) {
			if (slots[j] < 0) {
				slots[j] = (int)(s - mt->slots);
				break;
			}
		}
	}
}

int input_mt_assign_slots(struct input_dev *dev, int *slots,
			  const struct input_mt_pos *pos, int num_pos,
			  int dmax)
{
	struct input_mt *mt;
	int mu, nrc;
	uint64_t f;

	if (!dev || !dev->mt || !dev->mt->red)
		return -ENXIO;
	mt = dev->mt;
	if (num_pos > mt->num_slots)
		return -EINVAL;
	if (num_pos < 1)
		return 0;
	if (!slots || !pos)
		return -EINVAL;

	mu = (int)(2u * (unsigned int)dmax * (unsigned int)dmax);
	/* The slots' positions are the event path's cached values. */
	spin_lock_irqsave(&dev->event_lock, &f);
	nrc = input_mt_set_matrix(mt, pos, num_pos, mu);
	find_reduced_matrix(mt->red, num_pos, nrc / num_pos, nrc, mu);
	input_mt_set_slots(mt, slots, num_pos);
	spin_unlock_irqrestore(&dev->event_lock, f);
	return 0;
}

// ---------------------------------------------------------------------------
// For the event-device layer
// ---------------------------------------------------------------------------

int input_mt_get_values(struct input_dev *dev, unsigned int code, int first,
			int32_t *values, int max)
{
	struct input_mt *mt;
	uint64_t f;
	int i, n;

	if (!dev || !dev->mt || !input_is_mt_value((int)code))
		return -EINVAL;
	mt = dev->mt;
	if (first < 0 || first >= mt->num_slots || max <= 0)
		return 0;
	n = mt->num_slots - first < max ? mt->num_slots - first : max;
	spin_lock_irqsave(&dev->event_lock, &f);
	for (i = 0; i < n; i++)
		values[i] = input_mt_get_value(&mt->slots[first + i], code);
	spin_unlock_irqrestore(&dev->event_lock, f);
	return n;
}
