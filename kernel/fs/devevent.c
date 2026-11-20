// LikeOS -- /dev/devevents: device add, remove and change events.
//
// Drivers publish with devevent_publish() (see devevent.h) from whatever
// context they are in; the records go into one ring of the last
// DEVEVENT_RING_SIZE events (devevent_ring.h), and every open descriptor
// of /dev/devevents is a reader with its own cursor into it.
//
// Reading (the ABI comment in <kernel/uapi/devevent.h> is the contract):
//   - whole records only; a buffer smaller than one record is EINVAL, a
//     larger one gets as many as fit;
//   - a new descriptor starts at the next event published after its open;
//   - blocking unless O_NONBLOCK (EAGAIN), interruptible by signals;
//   - poll(): POLLIN while the reader has anything to read;
//   - a reader whose cursor fell out of the ring gets one OVERFLOW record
//     and continues with the oldest record still kept.
//
// Locking: the ring lock is a spinlock taken with interrupts off and never
// held across anything but copying records; readers copy records out under
// it into a small buffer on their stack and copy that to user memory after
// dropping it.  The waking is done after dropping the ring lock, with the
// same wait-queue wake an interrupt handler uses (poll_notify_wq), so a
// publisher may hold any lock and have interrupts off.  A per-reader mutex
// serialises reads on one descriptor (threads sharing it must not both
// deliver the same records); it is never held while waiting for events.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/fs/devevent.h>
#include <kernel/fs/devevent_ring.h>
#include <kernel/fs/device.h>
#include <kernel/fs/chrdev.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/waitq.h>
#include <kernel/ke/mutex.h>
#include <kernel/ke/uaccess.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/ke/cred.h>
#include <kernel/io/console.h>
#include <kernel/mm/memory.h>
#include <kernel/net/net.h>

/* /dev/devevents: a misc character device. */
#define DEVEVENT_MAJOR 10
#define DEVEVENT_MINOR 62

/* Records moved to user memory per round trip through the ring lock. */
#define DEVEVENT_READ_BATCH 8

/* Zero-initialised (bss): next_seq 0 reads as 1, see devevent_ring.h. */
static struct devevent_ring g_ring;
static spinlock_t g_ring_lock = SPINLOCK_INIT("devevent");
static struct wait_queue_head g_readers = WAIT_QUEUE_HEAD_INIT("devevent");

struct devevent_reader {
	/* The seq of the next record this reader gets.  Written with
	 * `lock' held, read without it by poll and the wait condition. */
	uint64_t cursor;
	struct mutex lock;
};

void devevent_publish(uint32_t type, uint32_t device_class,
		      uint64_t device_id, uint64_t parent_id, uint32_t flags,
		      uint16_t vendor, uint16_t product, uint32_t major,
		      uint32_t minor, const char *name)
{
	struct device_event ev;
	uint64_t fl;

	mm_memset(&ev, 0, sizeof(ev));
	ev.size = sizeof(ev);
	ev.version = DEVEVENT_VERSION;
	ev.type = type;
	ev.device_class = device_class;
	ev.device_id = device_id;
	ev.parent_id = parent_id;
	ev.timestamp_ns = hrtimer_now_ns();
	ev.flags = flags;
	ev.vendor = vendor;
	ev.product = product;
	ev.major = major;
	ev.minor = minor;
	for (unsigned i = 0; name && name[i] && i < sizeof(ev.name) - 1; i++)
		ev.name[i] = name[i];

	spin_lock_irqsave(&g_ring_lock, &fl);
	(void)devevent_ring_put(&g_ring, &ev);
	spin_unlock_irqrestore(&g_ring_lock, fl);
	poll_notify_wq(&g_readers);
}

static int devevent_reader_ready(struct devevent_reader *r)
{
	return devevent_ring_pending(&g_ring,
				     __atomic_load_n(&r->cursor,
						     __ATOMIC_ACQUIRE));
}

static int devevent_open(struct devfs_node *node, vfs_file_t *file, int flags,
			 struct task *cur)
{
	struct devevent_reader *r;
	uint64_t fl;

	(void)node;
	(void)flags;
	(void)cur;
	if (!capable())
		return -EPERM;
	r = kalloc(sizeof(*r));
	if (!r)
		return -ENOMEM;
	mm_memset(r, 0, sizeof(*r));
	mutex_init(&r->lock, "devevent-reader");
	spin_lock_irqsave(&g_ring_lock, &fl);
	r->cursor = devevent_ring_tail(&g_ring);
	spin_unlock_irqrestore(&g_ring_lock, fl);
	device_file_set_priv(file, r);
	return 0;
}

static void devevent_release(vfs_file_t *file)
{
	struct devevent_reader *r = device_file_priv(file);

	if (!r)
		return;
	device_file_set_priv(file, NULL);
	mutex_destroy(&r->lock);
	kfree(r);
}

/* Deliver what the reader has, up to `want' records, to `buf'.  Returns the
 * number of records delivered or -EFAULT (nothing delivered). */
static long devevent_deliver(struct devevent_reader *r, uint8_t *buf,
			     unsigned long want)
{
	struct device_event batch[DEVEVENT_READ_BATCH];
	unsigned long done = 0;

	while (done < want) {
		unsigned long room = want - done;
		uint64_t next, fl;
		unsigned n;

		if (room > DEVEVENT_READ_BATCH)
			room = DEVEVENT_READ_BATCH;
		spin_lock_irqsave(&g_ring_lock, &fl);
		n = devevent_ring_take(&g_ring, r->cursor, batch,
				       (unsigned)room, hrtimer_now_ns(), &next);
		spin_unlock_irqrestore(&g_ring_lock, fl);
		if (!n)
			break;
		/* The cursor moves only past what reached the reader: a
		 * fault leaves the records to the next read. */
		if (copy_to_user(buf + done * sizeof(struct device_event),
				 batch, n * sizeof(struct device_event)) != 0)
			return done ? (long)done : -EFAULT;
		__atomic_store_n(&r->cursor, next, __ATOMIC_RELEASE);
		done += n;
	}
	return (long)done;
}

static long devevent_read(vfs_file_t *file, void *buf, long bytes,
			  int nonblock)
{
	struct devevent_reader *r = device_file_priv(file);
	unsigned long want;

	if (!r)
		return -EBADF;
	if (bytes < (long)sizeof(struct device_event))
		return -EINVAL;
	want = (unsigned long)bytes / sizeof(struct device_event);
	for (;;) {
		long got;
		int rc;

		rc = mutex_lock_interruptible(&r->lock);
		if (rc)
			return -ERESTARTSYS;
		got = devevent_deliver(r, (uint8_t *)buf, want);
		mutex_unlock(&r->lock);
		if (got < 0)
			return got;
		if (got > 0)
			return got * (long)sizeof(struct device_event);
		if (nonblock)
			return -EAGAIN;
		rc = __wait_event_core(g_readers, devevent_reader_ready(r), 0,
				       WQ_WAIT_INTERRUPTIBLE);
		if (rc < 0)
			return rc;
	}
}

static short devevent_poll(vfs_file_t *file, short events,
			   struct poll_table *pt)
{
	struct devevent_reader *r = device_file_priv(file);

	poll_wait(pt, file, &g_readers);
	if (r && (events & POLLIN) && devevent_reader_ready(r))
		return POLLIN;
	return 0;
}

static const struct device_ops g_devevent_ops = {
	.open = devevent_open,
	.release = devevent_release,
	.read = devevent_read,
	.poll = devevent_poll,
};

static struct devfs_node g_devevent_node = {
	.path = "/dev/devevents",
	.mode = 0600,
	.uid = 0,
	.gid = 0,
	.major = DEVEVENT_MAJOR,
	.minor = DEVEVENT_MINOR,
	.ops = &g_devevent_ops,
};

void devevent_init(void)
{
	int r = chrdev_register(DEVEVENT_MAJOR, "misc");

	if (r < 0)
		kprintf("devevent: character major %u not registered (%d)\n",
			DEVEVENT_MAJOR, r);
	r = device_register(&g_devevent_node);
	if (r)
		kprintf("devevent: /dev/devevents not registered (%d)\n", r);
}
