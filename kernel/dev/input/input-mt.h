// LikeOS input core - multitouch slots
//
// The slot protocol: a multitouch device has a fixed number of slots, one
// per contact it can track at once.  A frame of contacts is reported slot by
// slot -- ABS_MT_SLOT selects one, then ABS_MT_TRACKING_ID says which
// contact lives there (a new id: a new contact; -1: the slot is empty now)
// and the ABS_MT_* axes say where it is.  Readers only ever see what
// changed: the core caches every slot's values and drops repeats, and it
// emits ABS_MT_SLOT only in front of a change to a different slot.
//
// Drivers use it like this, once per hardware frame:
//
//     for each contact c:
//         input_mt_slot(dev, slot of c);
//         if (input_mt_report_slot_state(dev, MT_TOOL_FINGER, c.touching)) {
//             input_report_abs(dev, ABS_MT_POSITION_X, c.x);
//             input_report_abs(dev, ABS_MT_POSITION_Y, c.y);
//         }
//     input_mt_sync_frame(dev);   // releases slots not reported (DROP_UNUSED),
//                                 // BTN_TOUCH, BTN_TOOL_*, ABS_X/Y emulation
//     input_sync(dev);
//
// A device whose contacts carry no identity of their own lets the core match
// them to slots by position (INPUT_MT_TRACK + input_mt_assign_slots()).
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) 2008-2010 Henrik Rydberg

#ifndef _KERNEL_DEV_INPUT_INPUT_MT_H_
#define _KERNEL_DEV_INPUT_INPUT_MT_H_

#include <kernel/dev/input/input.h>

/* input_mt_init_slots() flags. */
#define INPUT_MT_POINTER 0x0001 // pointer device (touchpad): BTN_TOOL_*
#define INPUT_MT_DIRECT 0x0002 // direct device (touchscreen)
#define INPUT_MT_DROP_UNUSED 0x0004 // release slots not seen in a frame
#define INPUT_MT_TRACK 0x0008 // core assigns slots by position
#define INPUT_MT_SEMI_MT 0x0010 // two contacts reported as a box
#define INPUT_MT_TOTAL_FORCE 0x0020 // ABS_PRESSURE: sum of all contacts'

/* Tracking ids run 0..INPUT_MT_TRKID_MAX and wrap. */
#define INPUT_MT_TRKID_MAX 0xffff
#define INPUT_MT_TRKID_SIGN ((INPUT_MT_TRKID_MAX + 1) >> 1)

/* The multitouch axes that are per slot: ABS_MT_TOUCH_MAJOR..ABS_MT_TOOL_Y.
 * ABS_MT_SLOT itself is not one of them. */
#define INPUT_MT_FIRST ABS_MT_TOUCH_MAJOR
#define INPUT_MT_LAST ABS_MT_TOOL_Y
#define INPUT_MT_NUM_VALUES (INPUT_MT_LAST - INPUT_MT_FIRST + 1)

struct input_mt_slot {
	int abs[INPUT_MT_NUM_VALUES]; // cached value of every MT axis
	unsigned int frame; // frame in which the slot was last reported
	unsigned int key; // driver's contact key (input_mt_get_slot_by_key)
};

struct input_mt {
	int trkid; // next tracking id to hand out
	int num_slots;
	int slot; // slot the next MT axis event goes to
	unsigned int flags;
	unsigned int frame; // current frame number
	/* INPUT_MT_TRACK: the reduced cost matrix of the contact/slot
	 * assignment (num_slots x num_slots), allocated once so that
	 * assigning never allocates. */
	int *red;
	struct input_mt_slot slots[];
};

/* A contact position, for input_mt_assign_slots(). */
struct input_mt_pos {
	int16_t x, y;
};

static inline int input_is_mt_value(int axis)
{
	return axis >= INPUT_MT_FIRST && axis <= INPUT_MT_LAST;
}

static inline int input_is_mt_axis(int axis)
{
	return axis == ABS_MT_SLOT || input_is_mt_value(axis);
}

static inline int input_mt_get_value(const struct input_mt_slot *slot,
				     unsigned int code)
{
	return slot->abs[code - INPUT_MT_FIRST];
}

static inline void input_mt_set_value(struct input_mt_slot *slot,
				      unsigned int code, int value)
{
	slot->abs[code - INPUT_MT_FIRST] = value;
}

/* A slot holds a contact (its tracking id is not -1). */
static inline int input_mt_is_active(const struct input_mt_slot *slot)
{
	return input_mt_get_value(slot, ABS_MT_TRACKING_ID) >= 0;
}

/* The slot was reported in the frame in progress. */
static inline int input_mt_is_used(const struct input_mt *mt,
				   const struct input_mt_slot *slot)
{
	return slot->frame == mt->frame;
}

/* Hand out the next tracking id (the counter wraps freely). */
static inline int input_mt_new_trkid(struct input_mt *mt)
{
	int id = mt->trkid & INPUT_MT_TRKID_MAX;

	mt->trkid = (int)((unsigned int)mt->trkid + 1u);
	return id;
}

/* Give the device `num_slots' (at most 1024) slots: ABS_MT_SLOT
 * (0..num_slots-1) and ABS_MT_TRACKING_ID (0..INPUT_MT_TRKID_MAX) are
 * declared, and with POINTER/DIRECT also BTN_TOUCH and
 * ABS_X/ABS_Y(/ABS_PRESSURE) copied from the MT position(/pressure) axes
 * -- so declare those first.  POINTER adds BTN_TOOL_FINGER..QUINTTAP up to
 * the slot count and INPUT_PROP_POINTER, DIRECT adds INPUT_PROP_DIRECT,
 * SEMI_MT INPUT_PROP_SEMI_MT.  Calling it again with the same count is a
 * no-op, with another one -EINVAL.  0, -EINVAL or -ENOMEM.  Process
 * context, before registration. */
int input_mt_init_slots(struct input_dev *dev, unsigned int num_slots,
			unsigned int flags);
/* Free the slots (a device that failed to register). */
void input_mt_destroy_slots(struct input_dev *dev);

/* Select the slot the following MT axis events describe. */
static inline void input_mt_slot(struct input_dev *dev, int slot)
{
	input_event(dev, EV_ABS, ABS_MT_SLOT, slot);
}

/* Report the current slot as holding a contact of `tool_type' (MT_TOOL_*)
 * or, with active false, as empty.  A slot that becomes active gets a new
 * tracking id; one that stays active keeps its id.  Returns active (and
 * false for a device without slots), so position reports can follow. */
int input_mt_report_slot_state(struct input_dev *dev, unsigned int tool_type,
			       int active);

static inline void input_mt_report_slot_inactive(struct input_dev *dev)
{
	input_mt_report_slot_state(dev, 0, 0);
}

/* BTN_TOOL_FINGER..BTN_TOOL_QUINTTAP for `count' contacts (exactly one of
 * them down, none for zero). */
void input_mt_report_finger_count(struct input_dev *dev, int count);
/* Single-touch emulation: BTN_TOUCH while any contact is active, ABS_X/ABS_Y
 * (and ABS_PRESSURE) from the oldest contact -- ABS_PRESSURE the sum of
 * all contacts' with INPUT_MT_TOTAL_FORCE -- and with use_count the
 * BTN_TOOL_* finger count. */
void input_mt_report_pointer_emulation(struct input_dev *dev, int use_count);
/* Release every active slot that was not reported in this frame, and start
 * a new frame. */
void input_mt_drop_unused(struct input_dev *dev);
/* End of a hardware frame: drop unused slots (INPUT_MT_DROP_UNUSED), report
 * the pointer emulation (finger count for POINTER devices that are not
 * SEMI_MT), start a new frame.  The driver still sends input_sync(). */
void input_mt_sync_frame(struct input_dev *dev);

/* Slot of the active contact with driver key `key'; otherwise a free slot
 * (empty and not used in this frame), which is given that key; -1 when all
 * are taken. */
int input_mt_get_slot_by_key(struct input_dev *dev, int key);
/* Match `num_pos' contact positions to slots: the active slots are matched
 * to the contacts by a balanced best match on the squared distances, with
 * every kept match closer than `dmax' (euclidean, in device units; 0: no
 * limit) where such an assignment exists; contacts left over get empty
 * slots, lowest first.  slots[i] receives the slot for pos[i].  0, -ENXIO
 * (no INPUT_MT_TRACK slots), -EINVAL (more contacts than slots).  The
 * driver serialises its frames (it runs from its interrupt handler). */
int input_mt_assign_slots(struct input_dev *dev, int *slots,
			  const struct input_mt_pos *pos, int num_pos,
			  int dmax);

/* For the event-device layer: the cached value of `code' in slots
 * first..first+max-1 (as far as there are slots), into values[], under
 * the event lock.  Returns the number copied (0 past the last slot), or
 * -EINVAL when the device has no slots or `code' is not a per-slot
 * axis. */
int input_mt_get_values(struct input_dev *dev, unsigned int code, int first,
			int32_t *values, int max);

#endif // _KERNEL_DEV_INPUT_INPUT_MT_H_
