// LikeOS-64 -- the kernel console as a display-manager client.
//
// Who owns the screen while the machine boots.
//
// Without a display-manager driver the console owns the hardware outright:
// it writes pixels into whatever framebuffer the firmware or the video
// driver handed it and tells that driver which rectangle changed.  That is
// still what happens on a machine with no driver for its display -- a
// ThinkPad on the UEFI framebuffer, say -- and nothing here runs.
//
// With one, that arrangement stops working the moment the driver takes the
// device somewhere the console cannot follow.  The scan-out moves (on the
// VMware device, to guest-backed screen targets), and the framebuffer the
// console is still faithfully painting is simply no longer what reaches the
// screen: the display goes black midway through boot on a machine that is
// otherwise running perfectly, all the way to a login prompt nobody can see.
//
// So where there IS a driver, the console becomes a client of it, exactly
// like the X server is: it gets a buffer object of its own, a framebuffer
// made from it, and a mode set that puts it on the screen; and its dirty
// rectangles go out through the driver's own dirty path, whatever that
// happens to be on this device.  One code path drives the display, and the
// console is above it rather than beside it.
//
// Handing the screen over and getting it back.  While a userspace master
// holds the device the console must not paint at all -- the pixels are not
// its own any more -- and when that master leaves, the CRTC is pointing at
// a framebuffer that has gone away, so the mode has to be set again to the
// console's own before anything it draws can appear.  drm_console_suspend()
// and drm_console_resume() are those two moments; the DRM core calls them
// when a master is set and dropped.
#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_internal.h>
#include <kernel/dev/video/fb.h>
#include <kernel/dev/video/fbdev.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/timer.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

/* One console, whichever device claims it first. */
static struct {
	struct drm_device *dev;
	struct drm_gem_object *obj;
	uint32_t fb_id;
	struct drm_mode_modeinfo mode;
	uint32_t w, h, pitch;

	int taken; /* the console is drawing into our buffer */
	int suspended; /* a userspace master holds the display */

	/* The rectangle the console has dirtied since the last push, and the
	 * lock that covers it.  Coalesced rather than queued: the console
	 * flushes per line, and one union is what the driver would end up
	 * doing with a queue of them anyway. */
	spinlock_t lock;
	int32_t x1, y1, x2, y2;
	int dirty;

	/* The thread that pushes.  Everything below is why it exists. */
	task_t *worker;
	volatile int worker_ready;
} g_con;

static uint8_t g_con_stack[16384] __attribute__((aligned(16)));

/* ---- pushing --------------------------------------------------------- */

/* Take whatever is dirty and hand it to the driver.
 *
 * One at a time: the push thread and whoever calls this directly (a mode
 * set, a resume, boot) would otherwise be inside the driver's dirty path
 * together, and that path has device state of its own -- a display surface,
 * a command buffer -- that is not written for two callers.  A push that
 * finds one in flight leaves the rectangle dirty; the thread's next tick
 * carries it. */
static volatile unsigned char g_pushing;

static void console_push(void)
{
	struct drm_mode_rect_k r;
	uint64_t fl;

	if (!g_con.taken || g_con.suspended)
		return;
	if (__atomic_test_and_set(&g_pushing, __ATOMIC_ACQUIRE))
		return;
	spin_lock_irqsave(&g_con.lock, &fl);
	if (!g_con.dirty) {
		spin_unlock_irqrestore(&g_con.lock, fl);
		__atomic_clear(&g_pushing, __ATOMIC_RELEASE);
		return;
	}
	r.x1 = g_con.x1;
	r.y1 = g_con.y1;
	r.x2 = g_con.x2;
	r.y2 = g_con.y2;
	g_con.dirty = 0;
	spin_unlock_irqrestore(&g_con.lock, fl);

	struct drm_device *dev = g_con.dev;
	struct drm_framebuffer *fb = drm_fb_lookup(dev, g_con.fb_id);
	if (fb && dev->ncrtc && dev->drv->fb_dirty)
		dev->drv->fb_dirty(dev, &dev->crtc[0], fb, &r, 1);
	__atomic_clear(&g_pushing, __ATOMIC_RELEASE);
}

/* The console's flush hook.  Runs wherever kprintf() does -- which is
 * everywhere, including interrupt handlers and code holding a spinlock with
 * interrupts off -- so it must not do anything that can sleep.
 *
 * The driver's dirty path can: on a guest-backed device it copies into a
 * mapped object and waits for the device to acknowledge a command buffer.
 * Doing that from a print under a spinlock is the deadlock this system
 * already learned about the hard way, so once there is a thread to do the
 * work, this only records what changed and wakes it.
 *
 * Before that thread runs -- which is most of boot, and every message
 * printed during it -- nothing else can run either, so the push happens
 * here and the output appears immediately.  That is the property that
 * matters for a boot log: it is worth nothing if it arrives after the
 * fault it was describing. */
static void console_flush_hook(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
	uint64_t fl;

	if (!g_con.taken || w == 0 || h == 0)
		return;
	int32_t x1 = (int32_t)x, y1 = (int32_t)y;
	int32_t x2 = (int32_t)(x + w), y2 = (int32_t)(y + h);

	spin_lock_irqsave(&g_con.lock, &fl);
	if (!g_con.dirty) {
		g_con.x1 = x1;
		g_con.y1 = y1;
		g_con.x2 = x2;
		g_con.y2 = y2;
		g_con.dirty = 1;
	} else {
		if (x1 < g_con.x1)
			g_con.x1 = x1;
		if (y1 < g_con.y1)
			g_con.y1 = y1;
		if (x2 > g_con.x2)
			g_con.x2 = x2;
		if (y2 > g_con.y2)
			g_con.y2 = y2;
	}
	spin_unlock_irqrestore(&g_con.lock, fl);

	if (g_con.worker_ready)
		sched_wake_channel_once(&g_con, 1);
	else
		console_push();
}

/* Mark the whole screen and get it on the display. */
static void console_push_all(void)
{
	uint64_t fl;

	spin_lock_irqsave(&g_con.lock, &fl);
	g_con.x1 = 0;
	g_con.y1 = 0;
	g_con.x2 = (int32_t)g_con.w;
	g_con.y2 = (int32_t)g_con.h;
	g_con.dirty = 1;
	spin_unlock_irqrestore(&g_con.lock, fl);
	console_push();
}

/* The pushing thread.
 *
 * It also wakes on a timer rather than only on the flush hook: a wake that
 * arrives while it is already running would otherwise be lost, and the
 * screen would sit one rectangle behind until the next print.  Fifty
 * milliseconds is below what an eye reads as lag and costs nothing when
 * there is nothing dirty. */
static void drm_console_worker(void *arg)
{
	(void)arg;
	g_con.worker_ready = 1;
	for (;;) {
		task_t *self = sched_current();

		if (self) {
			self->wait_channel = (void *)&g_con;
			self->wakeup_tick =
				timer_ticks() + (timer_get_frequency() / 20);
			self->state = TASK_BLOCKED;
			sched_schedule();
			self->wakeup_tick = 0;
			self->wait_channel = NULL;
			if (self->state != TASK_RUNNING)
				self->state = TASK_RUNNING;
		}
		/* A program that has /dev/fb0 open writes through its own
		 * mapping of this same buffer, so nothing marks a rectangle
		 * dirty and there is nothing for the loop above to find.  On
		 * a framebuffer in video memory the device noticed those
		 * writes by itself; a buffer object in RAM has no such
		 * mechanism, so the whole screen goes out on each tick for as
		 * long as the descriptor is open, and only then. */
		if (fbdev_display_owned() && !g_con.suspended)
			console_push_all();
		else
			console_push();
	}
}

/* ---- taking the console over ----------------------------------------- */

int drm_console_takeover(struct drm_device *dev)
{
	framebuffer_info_t fi, saved;
	int have_saved;

	if (g_con.taken)
		return -EBUSY;
	/* Both halves of the display path have to exist: something to set the
	 * mode with, and something to push a changed rectangle through. */
	if (!dev->drv->mode_set || !dev->drv->fb_dirty)
		return -ENODEV;
	if (dev->ncrtc == 0 || dev->nconn == 0 || dev->conn[0].nmodes == 0)
		return -ENODEV;

	spinlock_init(&g_con.lock, "drm_console");
	const struct drm_mode_modeinfo *mode = &dev->conn[0].modes[0];
	uint32_t w = mode->hdisplay, h = mode->vdisplay;
	uint32_t pitch = w * 4;

	if (w == 0 || h == 0)
		return -ENODEV;

	struct drm_gem_object *o =
		drm_gem_alloc(dev, DRM_GEM_BO, (uint64_t)pitch * h);
	if (!o)
		return -ENOMEM;
	if (drm_gem_alloc_pages_contig(o) != 0) {
		drm_gem_put(o);
		return -ENOMEM;
	}
	/* The backend's own view of the object -- a MOB, a GMR, whatever this
	 * device scans out of.  The ioctl paths do this for a client's
	 * buffers; there is no ioctl here. */
	if (dev->drv->gem_init && dev->drv->gem_init(o) != 0) {
		drm_gem_put(o);
		return -EIO;
	}

	uint32_t fb_id;
	if (drm_kms_fb_add_kernel(dev, o, w, h, pitch, &fb_id) != 0) {
		drm_gem_put(o);
		return -ENOSPC;
	}
	/* The framebuffer holds its own reference now. */
	drm_gem_put(o);

	g_con.dev = dev;
	g_con.obj = o;
	g_con.fb_id = fb_id;
	g_con.mode = *mode;
	g_con.w = w;
	g_con.h = h;
	g_con.pitch = pitch;

	if (drm_kms_crtc_set_kernel(dev, 0, mode, fb_id) != 0) {
		g_con.dev = NULL;
		return -EIO;
	}

	/* Where the console draws from here on. */
	have_saved = console_get_framebuffer_info(&saved) == 0;
	fi.framebuffer_base = phys_to_virt(o->pages[0]);
	fi.framebuffer_size = pitch * h;
	fi.horizontal_resolution = w;
	fi.vertical_resolution = h;
	fi.pixels_per_scanline = pitch / 4;
	fi.bytes_per_pixel = 4;

	fb_flush_hook_t old_hook = fb_get_flush_hook();
	fb_set_flush_hook(console_flush_hook);
	g_con.taken = 1;
	if (console_reinit_framebuffer(&fi) != 0) {
		/* Put back exactly what was there: the caller carries on with
		 * the display it already had. */
		g_con.taken = 0;
		fb_set_flush_hook(old_hook);
		if (have_saved)
			console_reinit_framebuffer(&saved);
		return -EIO;
	}
	console_push_all();

	kprintf("[drm] %s: console on KMS, %ux%u\n", dev->drv->name, w, h);
	return 0;
}

/* The thread cannot be created by the takeover above: the console goes onto
 * KMS while the machine is still bringing its devices up, which is before
 * there is a scheduler to add a task to.  The startup code calls this the
 * moment there is one.
 *
 * Everything printed in between is pushed inline, and that is safe for the
 * same reason it is necessary: nothing else is running yet. */
void drm_console_start_worker(void)
{
	if (!g_con.taken || g_con.worker)
		return;
	g_con.worker = sched_add_task(drm_console_worker, 0, g_con_stack,
				      sizeof(g_con_stack));
	if (!g_con.worker)
		kprintf("[drm] console push thread not started; "
			"pushing inline\n");
}

/* ---- handing the screen to a master and taking it back ---------------- */

void drm_console_suspend(struct drm_device *dev)
{
	if (!g_con.taken || g_con.dev != dev)
		return;
	g_con.suspended = 1;
}

void drm_console_resume(struct drm_device *dev)
{
	if (!g_con.taken || g_con.dev != dev)
		return;
	/* The master left the CRTC on a framebuffer of its own, which is
	 * being torn down behind it -- so this is a fresh mode set, not just
	 * a redraw.  What the console has to show is still in its buffer:
	 * nothing else could write there. */
	g_con.suspended = 0;
	if (drm_kms_crtc_set_kernel(dev, 0, &g_con.mode, g_con.fb_id) != 0) {
		kprintf("[drm] %s: console mode set failed after the display "
			"manager exited\n",
			dev->drv->name);
		return;
	}
	console_push_all();
}

int drm_console_active(const struct drm_device *dev)
{
	return g_con.taken && g_con.dev == dev;
}
