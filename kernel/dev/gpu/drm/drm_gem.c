// LikeOS-64 -- display-manager objects: buffers and surfaces, their handles,
// and sharing across processes (PRIME / dma-buf).
#include <kernel/dev/gpu/drm.h>
#include <kernel/uapi/drm/dma-buf.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/uaccess.h>
#include <kernel/fs/file.h>
#include <kernel/mm/memory.h>
#include <kernel/net/net.h>

/* Object ids map to mmap offsets: object k lives at offset (k+1) << 36, a
 * 64 GB window each, which no single object exceeds. */
#define DRM_MMAP_SHIFT 36

struct drm_gem_object *drm_gem_alloc(struct drm_device *dev,
				    enum drm_gem_kind kind, uint64_t size)
{
	struct drm_gem_object *o = kalloc(sizeof(*o));
	uint64_t fl;

	if (!o)
		return NULL;
	mm_memset(o, 0, sizeof(*o));
	o->refs = 1;
	o->dev = dev;
	o->kind = kind;
	o->size = (size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
	o->npages = (uint32_t)(o->size / PAGE_SIZE);
	spin_lock_irqsave(&dev->lock, &fl);
	o->id = ++dev->next_obj_id;
	o->next = dev->objects;
	dev->objects = o;
	spin_unlock_irqrestore(&dev->lock, fl);
	return o;
}

/* Make room for an object's frames before taking any.
 *
 * They come from the free list, which the page cache is allowed to grow into
 * until a few MB remain (its watermarks), and nothing but a page fault
 * reclaimed on the allocator's behalf.  A texture wants hundreds or thousands
 * of frames at once, so once the cache had filled RAM every glTexImage2D
 * failed -- Mesa says GL_OUT_OF_MEMORY -- while MemAvailable, which counts
 * the cache, told the panel, top and WebKit's pressure monitor that memory
 * was plentiful.  Objects are created from ioctls: process context, no
 * filesystem lock held, so reclaiming here is as safe as on a fault.
 *
 * The first few times it has to reclaim are logged, so a run that hits this
 * says so in dmesg instead of leaving the symptom to be guessed at. */
static void gem_make_room(uint32_t npages)
{
	uint64_t reclaimed = mm_reclaim_for_pages(npages);

	/* Counted by the reclaimer, not read off the free-page counter
	 * before and after: every other CPU moves that counter meanwhile,
	 * and a difference taken across the call once came out at -1 -- a
	 * concurrent allocation, printed unsigned, for a reclaim that had
	 * never run. */
	if (reclaimed) {
		static unsigned budget = 8;

		if (budget) {
			budget--;
			kprintf("drm: gem: reclaimed %lu page-cache pages for a %u-page object, %lu free now\n",
				(unsigned long)reclaimed, npages,
				(unsigned long)mm_get_free_pages());
		}
	}
}

int drm_gem_alloc_pages(struct drm_gem_object *o)
{
	if (o->pages || o->npages == 0)
		return 0;
	gem_make_room(o->npages);
	o->pages = kalloc(o->npages * sizeof(uint64_t));
	if (!o->pages)
		return -ENOMEM;
	for (uint32_t i = 0; i < o->npages; i++) {
		o->pages[i] = mm_allocate_physical_page_reclaim();
		if (!o->pages[i]) {
			for (uint32_t j = 0; j < i; j++)
				mm_free_physical_page(o->pages[j]);
			kfree(o->pages);
			o->pages = NULL;
			return -ENOMEM;
		}
		/* Zeroed, like the contiguous variant below: these pages are
		 * mmapped by clients and become device state (MOBs, COTables),
		 * so recycled kernel pages must not shine through -- that is
		 * both stale-state corruption on the device side and an
		 * information leak on the user side. */
		mm_memset(phys_to_virt(o->pages[i]), 0, PAGE_SIZE);
	}
	return 0;
}

/* Pages for an object the CPU has to see as ONE linear range.
 *
 * The console's framebuffer is written through a single base pointer with
 * plain stores, so its backing has to be virtually contiguous.  The direct
 * map is linear over physical memory, which makes a physically contiguous
 * run a virtually contiguous one; pages taken one at a time are neither.
 * Everything else about the object is unchanged -- drm_gem_put() frees the
 * frames one by one, which is what the allocator does with a run anyway. */
int drm_gem_alloc_pages_contig(struct drm_gem_object *o)
{
	if (o->pages || o->npages == 0)
		return 0;
	o->pages = kalloc(o->npages * sizeof(uint64_t));
	if (!o->pages)
		return -ENOMEM;
	uint64_t base = mm_allocate_contiguous_pages(o->npages);
	if (!base) {
		kfree(o->pages);
		o->pages = NULL;
		return -ENOMEM;
	}
	for (uint32_t i = 0; i < o->npages; i++)
		o->pages[i] = base + (uint64_t)i * PAGE_SIZE;
	mm_memset(phys_to_virt(base), 0, (size_t)o->npages * PAGE_SIZE);
	return 0;
}

void *drm_gem_page_virt(struct drm_gem_object *o, uint32_t page)
{
	if (!o->pages || page >= o->npages)
		return NULL;
	return phys_to_virt(o->pages[page]);
}

void drm_gem_get(struct drm_gem_object *o)
{
	__atomic_fetch_add(&o->refs, 1, __ATOMIC_ACQ_REL);
}

/* How many objects may sit waiting for the device before a free waits after
 * all.  The queue exists to move a wait off the drawing thread, not to let an
 * unbounded amount of memory sit unreclaimed: a client that destroys objects
 * far faster than the device retires them must still be made to slow down. */
#define DRM_GEM_DEAD_MAX 256

static void gem_destroy_final(struct drm_gem_object *o);

static void gem_reap(struct drm_device *dev, int poll)
{
	uint64_t fl;

	if (!dev || !__atomic_load_n(&dev->dead_n, __ATOMIC_RELAXED))
		return;
	/* Not from inside another reap: see dev->reaping.  What this one
	 * leaves behind, the walk already running collects on its next turn,
	 * and failing that the next submission does -- there are hundreds of
	 * those a second. */
	if (__atomic_exchange_n(&dev->reaping, 1, __ATOMIC_ACQUIRE))
		return;

	/* Asking the device costs a walk of every command-buffer slot through
	 * uncached memory, so it is done by the caller that is between frames
	 * anyway -- NOT by every drm_gem_put().  A submission drops a
	 * reference on each object it named, which is ten a submission and
	 * some thousands a second: polling the device on each of those turned
	 * a queue meant to REMOVE work from the drawing thread into a new cost
	 * on it.  Without the poll this still frees everything whose fence was
	 * already known to have passed. */
	if (poll && dev->drv && dev->drv->fence_poll)
		dev->drv->fence_poll(dev);

	for (;;) {
		struct drm_gem_object *o = NULL, **pp;

		spin_lock_irqsave(&dev->lock, &fl);
		for (pp = &dev->dead; *pp; pp = &(*pp)->dead_next) {
			if (!(*pp)->fence || (*pp)->fence->signaled) {
				o = *pp;
				*pp = o->dead_next;
				dev->dead_n--;
				break;
			}
		}
		spin_unlock_irqrestore(&dev->lock, fl);
		if (!o)
			break;
		if (o->fence) {
			drm_fence_put(o->fence);
			o->fence = NULL;
		}
		gem_destroy_final(o);
	}
	__atomic_store_n(&dev->reaping, 0, __ATOMIC_RELEASE);
}

/* The reaper thread.
 *
 * Destroying an object is not free -- a surface's teardown builds and queues
 * a DESTROY command, unbinds its MOB and hands the page-table pages back --
 * and a browser destroys hundreds of them a second.  All of that used to land
 * inside an execbuf ioctl, which is a thread trying to draw a frame.
 *
 * It does not belong there.  Nothing waits for a destroyed object, so the
 * work can happen on any thread at any time; put on a thread of its own it
 * costs a browser frame nothing at all, and on a machine with eight
 * processors it costs the machine nothing either.
 *
 * A plain 2ms tick rather than a wait queue: the only thing the timing
 * affects is how long a freed object's memory stays unreclaimed, the queue
 * has a cap for the case where that matters (DRM_GEM_DEAD_MAX), and a tick
 * cannot miss a wakeup. */
static struct drm_device *g_reap_dev;
static uint8_t g_reap_stack[16384] __attribute__((aligned(16)));

static void gem_reap_thread(void *arg)
{
	(void)arg;
	for (;;) {
		task_t *self = sched_current();

		if (self) {
			self->wait_channel = (void *)&g_reap_dev;
			self->wakeup_tick = timer_ticks() +
					    timer_ms_to_ticks(2) + 1;
			self->state = TASK_BLOCKED;
			sched_schedule();
			self->wakeup_tick = 0;
			self->wait_channel = NULL;
			if (self->state != TASK_RUNNING)
				self->state = TASK_RUNNING;
		}
		if (g_reap_dev)
			gem_reap(g_reap_dev, 0);
	}
}

void drm_gem_reap_start(struct drm_device *dev)
{
	if (g_reap_dev)
		return;
	g_reap_dev = dev;
	task_t *t = sched_add_task(gem_reap_thread, 0, g_reap_stack,
				   sizeof(g_reap_stack));
	if (t) {
		const char *nm = "drm-reap";
		unsigned i = 0;

		for (; nm[i] && i < sizeof(t->comm) - 1; i++)
			t->comm[i] = nm[i];
		t->comm[i] = '\0';
	}
}

void drm_gem_reap(struct drm_device *dev)
{
	/* Without asking the device.  Something already asks it several
	 * hundred times a second -- the driver's own fence poll, and every
	 * thread that waits -- so a queued object is seen to be free within a
	 * few milliseconds whatever this does, and nothing waits for it.
	 * Asking here as well was measurable cost for nothing. */
	gem_reap(dev, 0);
}

/* Wait for the oldest queued object and finish it, however long that takes.
 * Only for the cap above and for teardown; the ordinary path never waits. */
static void gem_reap_one_blocking(struct drm_device *dev)
{
	struct drm_gem_object *o;
	uint64_t fl;

	if (__atomic_exchange_n(&dev->reaping, 1, __ATOMIC_ACQUIRE))
		return; /* someone is already emptying it */
	spin_lock_irqsave(&dev->lock, &fl);
	o = dev->dead;
	if (o) {
		dev->dead = o->dead_next;
		dev->dead_n--;
	}
	spin_unlock_irqrestore(&dev->lock, fl);
	if (!o) {
		__atomic_store_n(&dev->reaping, 0, __ATOMIC_RELEASE);
		return;
	}
	if (o->fence) {
		/* Uninterruptibly.  The pages go back to the allocator below
		 * whatever happens here, so a wait that gives up because a
		 * signal is pending does not defer the free -- it performs it
		 * while the device is still reading, and the memory is handed
		 * to the next caller of the allocator to write over.  A
		 * browser is the worst case: its threads carry a pending
		 * signal almost continuously, so the interruptible form
		 * returned at once nearly every time it was called. */
		drm_fence_wait_flags(o->fence, 2000000000ULL, 0);
		drm_fence_put(o->fence);
		o->fence = NULL;
	}
	gem_destroy_final(o);
	__atomic_store_n(&dev->reaping, 0, __ATOMIC_RELEASE);
}

void drm_gem_put(struct drm_gem_object *o)
{
	if (!o)
		return;
	if (__atomic_sub_fetch(&o->refs, 1, __ATOMIC_ACQ_REL) != 0)
		return;
	struct drm_device *dev = o->dev;
	uint64_t fl;

	/* Whatever the device is already known to have finished with goes now,
	 * in this caller's time rather than in the time of the thread that
	 * happened to drop the last reference to it.  Without asking the
	 * device: see gem_reap(). */
	gem_reap(dev, 0);

	/* Still being read: queue it instead of standing here.  This wait --
	 * `gemfree' in the per-caller fencewait report -- was a quarter of the
	 * wall clock in a maximized browser and zero in a small window, which
	 * is what named it: nothing about destroying an object needs the
	 * thread that destroys it to stop drawing. */
	if (o->fence && !o->fence->signaled) {
		spin_lock_irqsave(&dev->lock, &fl);
		o->dead_next = dev->dead;
		dev->dead = o;
		dev->dead_n++;
		int over = dev->dead_n > DRM_GEM_DEAD_MAX;
		spin_unlock_irqrestore(&dev->lock, fl);
		if (over)
			gem_reap_one_blocking(dev);
		return;
	}
	if (o->fence) {
		drm_fence_put(o->fence);
		o->fence = NULL;
	}
	gem_destroy_final(o);
}

static void gem_destroy_final(struct drm_gem_object *o)
{
	struct drm_device *dev = o->dev;
	uint64_t fl;

	/* dev->drv is NULL until drm_dev_register() attaches the backend, and
	 * a backend may build objects of its own before it registers.  Such
	 * an object was never handed to the backend -- there is nothing for
	 * it to free -- and the rest of this function still holds. */
	if (dev->drv && dev->drv->gem_free)
		dev->drv->gem_free(o);
	spin_lock_irqsave(&dev->lock, &fl);
	struct drm_gem_object **pp = &dev->objects;
	while (*pp) {
		if (*pp == o) {
			*pp = o->next;
			break;
		}
		pp = &(*pp)->next;
	}
	spin_unlock_irqrestore(&dev->lock, fl);
	if (o->pages) {
		/* If the driver gave these pages to its device, only the
		 * driver knows when the device has stopped reaching them --
		 * and freeing them early is invisible from this side, because
		 * the writes that follow are the device's, not a processor's.
		 * See gem_release_pages(). */
		if (dev->drv && dev->drv->gem_release_pages) {
			dev->drv->gem_release_pages(o);
		} else {
			for (uint32_t i = 0; i < o->npages; i++)
				if (o->pages[i])
					mm_free_physical_page(o->pages[i]);
		}
		kfree(o->pages);
	}
	kfree(o);
}

/* ---- mmap offsets ------------------------------------------------------ */
//
// The offset a client mmaps on the device node is not a file position, it
// names an object: each object owns a 4GB-aligned window addressed by its
// device-global id.  The window is wider than any object can be, the id is
// never reused for the object's lifetime, and both directions below are the
// only place the encoding exists -- every ioctl that reports a map handle
// asks here, so the scheme is free to be this simple.

uint64_t drm_gem_mmap_offset(struct drm_gem_object *o)
{
	return (uint64_t)o->id << 32;
}

/* Object by mmap offset (a reference), for the device node's mmap. */
/* Take a reference only if the object still has one.
 *
 * A lookup walks the device's list, which an object stays on until its
 * teardown finishes -- and its teardown may now be waiting for the device
 * (see drm_gem_reap).  A plain drm_gem_get() there resurrects an object whose
 * last reference has already gone and whose memory is about to be handed
 * back.  The window was always open; queueing the teardown widens it from
 * microseconds to milliseconds, which is what makes it worth closing. */
int drm_gem_get_unless_zero(struct drm_gem_object *o)
{
	int r = __atomic_load_n(&o->refs, __ATOMIC_RELAXED);

	while (r != 0) {
		if (__atomic_compare_exchange_n(&o->refs, &r, r + 1, 1,
						__ATOMIC_ACQ_REL,
						__ATOMIC_RELAXED))
			return 1;
	}
	return 0;
}

struct drm_gem_object *drm_gem_by_offset(struct drm_device *dev,
					 uint64_t offset)
{
	uint32_t id = (uint32_t)(offset >> 32);
	struct drm_gem_object *found = NULL;
	uint64_t fl;

	spin_lock_irqsave(&dev->lock, &fl);
	for (struct drm_gem_object *o = dev->objects; o; o = o->next) {
		if (o->id == id && drm_gem_get_unless_zero(o)) {
			found = o;
			break;
		}
	}
	spin_unlock_irqrestore(&dev->lock, fl);
	return found;
}

/* ---- handles -------------------------------------------------------- */

/* Handles name the file they belong to.
 *
 * A client that shares a surface passes the id its own file holds and the
 * other side is expected to find the same object -- which a bare per-file
 * index cannot do, because every file has an index 20.  Carrying the file
 * in the upper half makes an id mean one object device-wide, so a
 * reference by id can be resolved whoever created it, while every ordinary
 * lookup stays the same array index it was. */
#define DRM_HANDLE_SLOT_BITS 16
#define DRM_HANDLE_SLOT_MASK ((1u << DRM_HANDLE_SLOT_BITS) - 1)

static uint32_t drm_handle_make(struct drm_file *fp, uint32_t slot)
{
	return (fp->file_id << DRM_HANDLE_SLOT_BITS) | slot;
}

/* The handle naming a slot this file already holds. */
uint32_t drm_gem_handle_of_slot(struct drm_file *fp, uint32_t slot)
{
	return drm_handle_make(fp, slot);
}

int drm_gem_handle_create(struct drm_file *fp, struct drm_gem_object *o,
			  uint32_t *handle_out)
{
	uint64_t fl;

	/* The table has to be grown without the lock -- kalloc may sleep --
	 * and that window is where this used to go wrong.  The old code
	 * dropped the lock, allocated, retook it, and then carried on using
	 * the capacity it had read BEFORE the window, without checking
	 * whether anything had changed.  Two threads growing the table at the
	 * same moment therefore both installed their own replacement:
	 *
	 *   - the second overwrote the first's table pointer, so the slot the
	 *     first had just handed out was silently reused for the second's
	 *     object, and a handle already returned to userspace now named
	 *     something else entirely; and
	 *   - the copy into the new table was sized from the CURRENT
	 *     nhandles, which the first thread had already enlarged, so a
	 *     third grower could memcpy more entries than the buffer it had
	 *     allocated could hold.
	 *
	 * The client sees this as a valid surface handle that stops
	 * resolving, or resolves to an object of the wrong kind, some time
	 * after it was issued -- the command referencing it is refused, the
	 * driver abandons the batch and carries on with state the device
	 * never received, and the crash lands somewhere else entirely.  It
	 * needs two threads to grow the table in the same instant, which is
	 * why it only shows up on pages that create surfaces in bursts.
	 *
	 * So the capacity the decision was made on is re-checked once the
	 * lock is back: if it moved, this attempt is void -- drop the new
	 * chunk and start again, because the winner's chunk may already have
	 * the free slot this call wanted.
	 *
	 * The table grows one chunk at a time and the old chunks stay where
	 * they are, so growing never copies anything and never asks for more
	 * than a page of pointers -- a table that doubled needed a contiguous
	 * allocation of its full size each time, which the kernel allocator
	 * cannot reclaim page cache for, and at 65536 slots that is half a
	 * megabyte. */
	for (;;) {
		spin_lock_irqsave(&fp->lock, &fl);
		/* Slot 0 is never issued; nothing below the hint is free. */
		if (fp->handle_hint == 0)
			fp->handle_hint = 1;
		for (uint32_t h = fp->handle_hint; h < fp->nhandles; h++) {
			struct drm_gem_object **slot = drm_handle_slot(fp, h);

			if (!*slot) {
				drm_gem_get(o);
				*slot = o;
				fp->handle_hint = h + 1;
				spin_unlock_irqrestore(&fp->lock, fl);
				*handle_out = drm_handle_make(fp, h);
				return 0;
			}
		}
		/* Grow by a chunk. */
		uint32_t oldcap = fp->nhandles;

		if (oldcap >= DRM_MAX_HANDLES) {
			/* Full.  Say so once per file, with what the slots
			 * hold: the client sees only an out-of-memory error
			 * from its GL library, with most of RAM free, and
			 * the split between surfaces and buffers is what
			 * tells a leak from a page that really uses this
			 * many. */
			uint32_t nsurf = 0, nbo = 0, nother = 0;
			int report = !fp->handles_full_reported;

			if (report) {
				fp->handles_full_reported = 1;
				for (uint32_t h = 1; h < oldcap; h++) {
					struct drm_gem_object *e =
						*drm_handle_slot(fp, h);

					if (!e)
						continue;
					if (e->kind == DRM_GEM_SURFACE)
						nsurf++;
					else if (e->kind == DRM_GEM_BO)
						nbo++;
					else
						nother++;
				}
			}
			spin_unlock_irqrestore(&fp->lock, fl);
			if (report)
				kprintf("drm: gem: handle table full for pid %d: %u slots hold %u surfaces, %u buffers, %u other\n",
					sched_current() ? (int)sched_current()->id : -1,
					(unsigned)DRM_MAX_HANDLES, nsurf, nbo,
					nother);
			return -ENOSPC;
		}
		spin_unlock_irqrestore(&fp->lock, fl);

		struct drm_gem_object **chunk =
			kalloc(DRM_HANDLE_CHUNK * sizeof(*chunk));

		if (!chunk)
			return -ENOMEM;
		mm_memset(chunk, 0, DRM_HANDLE_CHUNK * sizeof(*chunk));

		spin_lock_irqsave(&fp->lock, &fl);
		if (fp->nhandles != oldcap) {
			/* Somebody else grew it while we were allocating. */
			spin_unlock_irqrestore(&fp->lock, fl);
			/* Freed outside the lock: kfree can fire a
			 * TLB-shootdown IPI, which must not happen with
			 * interrupts disabled. */
			kfree(chunk);
			continue;
		}
		fp->handles[oldcap / DRM_HANDLE_CHUNK] = chunk;
		fp->nhandles = oldcap + DRM_HANDLE_CHUNK;
		/* The first slot the table did not have.  Slot 0 is never
		 * issued, so a first allocation starts at 1. */
		uint32_t h = oldcap ? oldcap : 1;

		drm_gem_get(o);
		*drm_handle_slot(fp, h) = o;
		fp->handle_hint = h + 1;
		spin_unlock_irqrestore(&fp->lock, fl);
		*handle_out = drm_handle_make(fp, h);
		return 0;
	}
}

/* This file's own handle, and nothing else: the id has to name this file. */
struct drm_gem_object *drm_gem_lookup(struct drm_file *fp, uint32_t handle)
{
	uint64_t fl;
	struct drm_gem_object *o = NULL;
	uint32_t slot = handle & DRM_HANDLE_SLOT_MASK;

	if (!handle || (handle >> DRM_HANDLE_SLOT_BITS) != fp->file_id)
		return NULL;
	spin_lock_irqsave(&fp->lock, &fl);
	if (slot && slot < fp->nhandles) {
		o = *drm_handle_slot(fp, slot);
		if (o)
			drm_gem_get(o);
	}
	spin_unlock_irqrestore(&fp->lock, fl);
	return o;
}

/* An id belonging to ANOTHER file on the same device.
 *
 * This is how one process references a surface another created and told it
 * about; the caller decides whether it is entitled to (see the surface
 * reference path).  The device lock is held while the owning file is found
 * and the reference taken, so a file closing concurrently either has not
 * been unlinked yet -- and the object is referenced before it can go -- or
 * has, and the lookup misses. */
struct drm_gem_object *drm_gem_lookup_foreign(struct drm_device *dev,
					      uint32_t handle)
{
	uint32_t owner = handle >> DRM_HANDLE_SLOT_BITS;
	uint32_t slot = handle & DRM_HANDLE_SLOT_MASK;
	struct drm_gem_object *o = NULL;
	uint64_t fl;

	if (!handle || !slot)
		return NULL;
	spin_lock_irqsave(&dev->lock, &fl);
	for (struct drm_file *f = dev->files; f; f = f->next) {
		if (f->file_id != owner)
			continue;
		if (slot < f->nhandles) {
			o = *drm_handle_slot(f, slot);
			if (o)
				drm_gem_get(o);
		}
		break;
	}
	spin_unlock_irqrestore(&dev->lock, fl);
	return o;
}

int drm_gem_handle_delete(struct drm_file *fp, uint32_t handle)
{
	uint64_t fl;
	struct drm_gem_object *o = NULL;

	uint32_t slot = handle & DRM_HANDLE_SLOT_MASK;

	if (!handle || (handle >> DRM_HANDLE_SLOT_BITS) != fp->file_id)
		return -EINVAL;
	spin_lock_irqsave(&fp->lock, &fl);
	if (slot && slot < fp->nhandles) {
		struct drm_gem_object **cell = drm_handle_slot(fp, slot);

		o = *cell;
		*cell = NULL;
		if (o && slot < fp->handle_hint)
			fp->handle_hint = slot;
	}
	spin_unlock_irqrestore(&fp->lock, fl);
	if (!o)
		return -EINVAL;
	drm_gem_put(o);
	return 0;
}

/* ---- PRIME: dma-buf descriptors ----------------------------------------- */

struct dmabuf_ctx {
	struct drm_gem_object *obj;
};

static uint64_t dmabuf_page_phys(void *obj, uint64_t index)
{
	struct drm_gem_object *o = obj;
	return o->dev->drv->gem_page_phys ? o->dev->drv->gem_page_phys(o, index) :
					     0;
}

/* Region-record get/put for descriptor mappings; census as in drm_drv.c. */
static void dmabuf_obj_get(void *obj)
{
	drm_gem_get(obj);
}

static void dmabuf_obj_put(void *obj)
{
	drm_gem_put(obj);
}

/* Descriptor mappings address the object from byte zero of the
 * descriptor, so their record offsets are object offsets already -- which
 * is why they carry their own callbacks: the sweep tells the two flavours
 * of record apart by the callback pointer and places each by its own base
 * (mm_dirty_walk.ops_alt).
 *
 * They used to be left out of the sweep on purpose, on the theory that a
 * descriptor mapping was a rare cross-process case best answered by
 * reporting the whole object.  It is the ordinary case: a browser's web
 * process paints its tiles through exactly such a mapping and draws them
 * from the same process, and every one of its submissions re-sent the
 * whole tile to the device -- two megabytes a frame that nothing had
 * written -- because the record was in plain sight and not looked at. */
static void dmabuf_dirty_mkwrite(void *obj, uint64_t offset)
{
	drm_gem_dirty_fault_page(obj, offset / PAGE_SIZE);
}

static void dmabuf_dirty_page_dirty(void *obj, uint64_t offset)
{
	drm_gem_dirty_mark_page(obj, offset / PAGE_SIZE);
}

static int dmabuf_dirty_wp_new(void *obj)
{
	return drm_gem_dirty_wp_new_mapping(obj);
}

const struct mm_dirty_ops drm_gem_dmabuf_dirty_ops = {
	.mkwrite = dmabuf_dirty_mkwrite,
	.page_dirty = dmabuf_dirty_page_dirty,
	.wp_new_mapping = dmabuf_dirty_wp_new,
	.map_census = drm_gem_dirty_map_census,
};

/* A mapping made through the descriptor rather than the device node: it
 * writes the same pages, so it counts. */
static int dmabuf_mmap(vfs_file_t *f, struct device_mmap *m)
{
	struct dmabuf_ctx *c = device_file_priv(f);
	struct drm_gem_object *o = c->obj;

	if (!o->dev->drv->gem_page_phys)
		return -ENODEV;
	if (m->offset + m->length > o->size)
		return -EINVAL;
	m->page_phys = dmabuf_page_phys;
	m->obj = o;
	m->get = dmabuf_obj_get;
	m->put = dmabuf_obj_put;
	m->pte_extra = o->dev->drv->gem_mmap_pte_extra;
	m->dirty_ops = &drm_gem_dmabuf_dirty_ops;
	drm_gem_get(o); /* the mapping's reference */
	return 0;
}

static short dmabuf_poll(vfs_file_t *f, short events, struct poll_table *pt)
{
	struct dmabuf_ctx *c = device_file_priv(f);
	struct drm_fence *fence = c->obj->fence;

	if (fence) {
		poll_wait(pt, f, &fence->wq);
		if (!fence->signaled)
			return 0;
	}
	return events & (POLLIN | POLLOUT);
}

static long dmabuf_ioctl(vfs_file_t *f, unsigned long req, void *argp,
			 struct task *cur)
{
	struct dmabuf_ctx *c = device_file_priv(f);
	(void)cur;

	if (_IOC_TYPE(req) != DMA_BUF_BASE)
		return -ENOTTY;
	switch (_IOC_NR(req)) {
	case 0: { /* DMA_BUF_IOCTL_SYNC: wait for the device before CPU use */
		struct dma_buf_sync s;
		if (copy_from_user(&s, argp, sizeof(s)) != 0)
			return -EFAULT;
		/* Uninterruptible: this ioctl exists to promise the caller the
		 * device is finished before it touches the mapping, and it
		 * reports nothing back.  Returning early on a signal would
		 * hand back that promise unkept. */
		if (!(s.flags & DMA_BUF_SYNC_END) && c->obj->fence)
			drm_fence_wait_flags(c->obj->fence, 2000000000ULL, 0);
		return 0;
	}
	case 1: /* DMA_BUF_SET_NAME */
		return 0;
	default:
		return -ENOTTY;
	}
}

static int dmabuf_fstat(vfs_file_t *f, struct kstat *st)
{
	struct dmabuf_ctx *c = device_file_priv(f);
	st->st_size = c->obj->size;
	return 0;
}

static void dmabuf_release(vfs_file_t *f)
{
	struct dmabuf_ctx *c = device_file_priv(f);

	if (c) {
		drm_gem_put(c->obj);
		kfree(c);
	}
}

static const struct device_ops dmabuf_ops = {
	.mmap = dmabuf_mmap,
	.poll = dmabuf_poll,
	.ioctl = dmabuf_ioctl,
	.fstat = dmabuf_fstat,
	.release = dmabuf_release,
};

int drm_prime_export(struct drm_file *fp, struct drm_gem_object *o, int flags)
{
	task_t *cur = sched_current();
	struct dmabuf_ctx *c = kalloc(sizeof(*c));
	(void)fp;

	if (!c)
		return -ENOMEM;
	drm_gem_get(o);
	c->obj = o;
	vfs_file_t *file = device_anon_file(&dmabuf_ops, c, "dmabuf",
					    (flags & O_RDWR) ? O_RDWR : O_RDONLY);
	if (!file) {
		drm_gem_put(o);
		kfree(c);
		return -ENOMEM;
	}
	file->refcount = 1;
	int fd = fd_install(cur, file);
	if (fd < 0) {
		vfs_close(file);
		return fd;
	}
	if (flags & O_CLOEXEC)
		task_set_fd_flags(cur, (unsigned)fd, FD_CLOEXEC);
	return fd;
}

struct drm_gem_object *drm_prime_import(int fd)
{
	task_t *cur = sched_current();
	vfs_file_t *f = fdget(cur, fd);

	if (!f)
		return NULL;
	if (device_file_ops(f) != &dmabuf_ops) {
		fdput(f);
		return NULL;
	}
	struct dmabuf_ctx *c = device_file_priv(f);
	struct drm_gem_object *o = c->obj;
	drm_gem_get(o);
	fdput(f);
	return o;
}
