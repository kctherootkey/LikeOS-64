// LikeOS-64 fbdev core - backend-neutral /dev/fb0 implementation
//
// Display servers (X.org fbdev/modesetting) drive the screen through this
// interface: FBIOGET_* for geometry discovery, mmap of the framebuffer for
// pixel access, FBIOPUT_VSCREENINFO for mode changes.  When the VMware SVGA
// II driver owns the display, mode changes route through its runtime modeset
// path (console included); on the GOP fallback the single boot mode is the
// only accepted mode.

#include <kernel/dev/input/mouse.h>
#include <kernel/dev/video/fb.h>
#include <kernel/dev/video/fbdev.h>
#include <kernel/dev/video/vmsvga2.h>
#include <kernel/uapi/fb.h>
#include <kernel/uapi/bug.h>
#include <kernel/io/console.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>

// SMAP-aware user copies (argp/user_buf are raw user pointers)
static int fbdev_copy_to_user(void *user_dst, const void *src, size_t len)
{
	if (!user_dst)
		return -EFAULT;
	smap_disable();
	kmemcpy(user_dst, src, len);
	smap_enable();
	return 0;
}

static int fbdev_copy_from_user(void *dst, const void *user_src, size_t len)
{
	if (!user_src)
		return -EFAULT;
	smap_disable();
	kmemcpy(dst, user_src, len);
	smap_enable();
	return 0;
}

// Current scanout geometry from the active backend.  Returns 0 on success.
static int fbdev_current_info(framebuffer_info_t *fi)
{
	if (vmsvga2_active())
		return vmsvga2_get_info(fi);
	return console_get_framebuffer_info(fi);
}

uint64_t fbdev_get_phys(uint64_t *size_out)
{
	framebuffer_info_t fi;

	if (vmsvga2_active())
		return vmsvga2_get_fb_phys(size_out);
	if (console_get_framebuffer_info(&fi) != 0 || !fi.framebuffer_base)
		return 0;
	if (size_out)
		*size_out = fi.framebuffer_size;
	if (is_direct_map_addr((uint64_t)fi.framebuffer_base))
		return virt_to_phys(fi.framebuffer_base);
	// Early identity-mapped address: VA == PA
	return (uint64_t)fi.framebuffer_base;
}

uint64_t fbdev_mmap_phys(uint64_t offset, uint64_t length)
{
	uint64_t size = 0;
	uint64_t phys = fbdev_get_phys(&size);

	if (!phys || length == 0)
		return 0;
	if (offset & (PAGE_SIZE - 1))
		return 0; // mmap offsets are page-granular
	if (offset >= size || length > size - offset)
		return 0;
	/* A mapping client (X.org fbdev style) scans out by storing straight
	 * to VRAM and sends no update commands.  Enable SVGA traces so the
	 * host snoops those writes; sticky-on - the console's explicit
	 * update-rect path remains correct alongside it, at a small
	 * host-side tracking cost once a client has mapped the fb. */
	vmsvga2_set_traces(1);
	return phys + offset;
}

static void fbdev_fill_var(struct fb_var_screeninfo *var,
			   const framebuffer_info_t *fi)
{
	kmemset(var, 0, sizeof(*var));
	var->xres = fi->horizontal_resolution;
	var->yres = fi->vertical_resolution;
	var->xres_virtual = fi->pixels_per_scanline;
	var->yres_virtual = fi->vertical_resolution;
	var->bits_per_pixel = fi->bytes_per_pixel * 8;
	var->activate = FB_ACTIVATE_NOW;
	var->vmode = FB_VMODE_NONINTERLACED;
	// 60 Hz nominal pixel clock (informational on virtual hardware)
	if (var->xres && var->yres) {
		uint64_t px = (uint64_t)var->xres * var->yres * 60;
		var->pixclock = px ? (uint32_t)(1000000000000ULL / px) : 0;
	}
	if (var->bits_per_pixel == 16) {
		// RGB565
		var->red.offset = 11;
		var->red.length = 5;
		var->green.offset = 5;
		var->green.length = 6;
		var->blue.offset = 0;
		var->blue.length = 5;
	} else {
		// XRGB8888
		var->red.offset = 16;
		var->red.length = 8;
		var->green.offset = 8;
		var->green.length = 8;
		var->blue.offset = 0;
		var->blue.length = 8;
	}
}

static void fbdev_fill_fix(struct fb_fix_screeninfo *fix,
			   const framebuffer_info_t *fi)
{
	static const char id_svga[] = "svga2";
	static const char id_gop[] = "gopfb";
	const char *id = vmsvga2_active() ? id_svga : id_gop;
	uint64_t size = 0;
	uint64_t phys = fbdev_get_phys(&size);
	int i;

	kmemset(fix, 0, sizeof(*fix));
	for (i = 0; id[i] && i < 15; i++)
		fix->id[i] = id[i];
	fix->smem_start = (unsigned long)phys;
	fix->smem_len = (uint32_t)size;
	fix->type = FB_TYPE_PACKED_PIXELS;
	fix->visual = FB_VISUAL_TRUECOLOR;
	fix->line_length = fi->pixels_per_scanline * fi->bytes_per_pixel;
	fix->accel = FB_ACCEL_NONE;
}

// FBIOPUT_VSCREENINFO: runtime mode change (SVGA backend only)
static int fbdev_put_var(const struct fb_var_screeninfo *var)
{
	framebuffer_info_t fi;

	if (fbdev_current_info(&fi) != 0)
		return -ENODEV;

	if (var->xres == 0 || var->yres == 0)
		return -EINVAL;
	if (var->bits_per_pixel != 32 && var->bits_per_pixel != 16)
		return -EINVAL;

	// No-op when the requested mode is already active.
	if (var->xres == fi.horizontal_resolution &&
	    var->yres == fi.vertical_resolution &&
	    var->bits_per_pixel == fi.bytes_per_pixel * 8)
		return 0;

	if (var->activate & FB_ACTIVATE_TEST) {
		if (!vmsvga2_active())
			return -EINVAL; // GOP: only the current mode exists
		if (var->xres > vmsvga2_get_max_width() ||
		    var->yres > vmsvga2_get_max_height())
			return -EINVAL;
		if ((uint64_t)var->xres * var->yres *
			    (var->bits_per_pixel / 8) >
		    vmsvga2_get_vram_size())
			return -EINVAL;
		return 0;
	}

	if (!vmsvga2_active())
		return -EINVAL; // GOP framebuffer cannot change modes

	if (vmsvga2_set_mode(var->xres, var->yres, var->bits_per_pixel) != 0)
		return -EINVAL;
	if (WARN_ON_ONCE(vmsvga2_get_info(&fi) != 0))
		return -EIO;
	// Cascade the new geometry through fb/console/tty (SIGWINCH).
	if (console_reinit_framebuffer(&fi) != 0)
		return -EIO;
	vmsvga2_update_full();
	return 0;
}

/*
 * How many handles are open on /dev/fb0.
 *
 * A process that maps the framebuffer draws straight into video memory, behind
 * the console's back.  The console keeps its own back buffer, so the text is
 * never lost -- but nothing tells it to put the text back on screen when the
 * other program is done, and on a system with no virtual terminals to switch
 * between there is no other moment at which that would happen.  Killing an X
 * server left the screen showing whatever it had drawn last, with a working
 * but invisible shell behind it.
 *
 * So the last close is the signal.  Counted rather than tracked per handle
 * because a display server has several (it dup()s, and it survives fork), and
 * the console must not be redrawn while one of them is still drawing.
 */
static int g_fb0_opens;

/*
 * Is another program driving the display?
 *
 * While one is, the console must not paint: it and the display server would be
 * writing to the same pixels, and the console would win whenever anything --
 * a kernel message, an echoed keystroke -- made it draw.  On a system with
 * virtual terminals this is what KD_GRAPHICS does; the open count is the same
 * signal without the VT.
 *
 * The console keeps updating its BACK buffer throughout, so nothing written
 * meanwhile is lost: the full redraw on the last close brings it all back.
 */
int fbdev_display_owned(void)
{
	return g_fb0_opens > 0;
}

void fbdev_opened(void)
{
	g_fb0_opens++;
}

void fbdev_closed(void)
{
	if (g_fb0_opens > 0)
		g_fb0_opens--;
	if (g_fb0_opens > 0)
		return;

	/* Every pixel is suspect, so mark the whole screen rather than any
	 * region the console thinks it dirtied -- it has no idea what was
	 * drawn over it.  Flushed directly rather than through
	 * console_flush(), which rate-limits and would drop this one. */
	fb_mark_full_dirty();
	fb_flush_dirty_regions();

	/* And the pointer, which was not drawn either while the display was
	 * owned.  It needs more than a redraw: the background it saved is from
	 * before the other program took over, so it is discarded first --
	 * otherwise the first movement stamps a rectangle of pre-session pixels
	 * onto the console that was just restored. */
	mouse_console_display_released();
}

int fbdev_ioctl(unsigned long req, void *argp, struct task *cur)
{
	framebuffer_info_t fi;

	(void)cur;
	if (fbdev_current_info(&fi) != 0)
		return -ENODEV;
	if (!argp && req != FBIOBLANK)
		return -EFAULT;

	switch (req) {
	case FBIOGET_VSCREENINFO: {
		struct fb_var_screeninfo var;
		fbdev_fill_var(&var, &fi);
		return fbdev_copy_to_user(argp, &var, sizeof(var));
	}
	case FBIOGET_FSCREENINFO: {
		struct fb_fix_screeninfo fix;
		fbdev_fill_fix(&fix, &fi);
		return fbdev_copy_to_user(argp, &fix, sizeof(fix));
	}
	case FBIOPUT_VSCREENINFO: {
		struct fb_var_screeninfo var;
		int rc = fbdev_copy_from_user(&var, argp, sizeof(var));
		if (rc != 0)
			return rc;
		return fbdev_put_var(&var);
	}
	case FBIOBLANK: {
		long level = (long)(uint64_t)argp;
		if (vmsvga2_active())
			return vmsvga2_display_enable(level ==
						      FB_BLANK_UNBLANK) == 0 ?
				       0 :
				       -EIO;
		return 0; // GOP: blanking not supported, pretend success
	}
	case FBIOGETCMAP:
	case FBIOPUTCMAP:
	case FBIOPAN_DISPLAY:
		return -EINVAL; // truecolor visual, no panning
	default:
		return -ENOTTY;
	}
}

// read()/write() on /dev/fb0: linear access to the framebuffer.
long fbdev_read(uint64_t pos, void *user_buf, long bytes)
{
	framebuffer_info_t fi;
	uint8_t *base;

	if (fbdev_current_info(&fi) != 0)
		return -ENODEV;
	base = (uint8_t *)fi.framebuffer_base;
	if (!base || bytes < 0)
		return -EINVAL;
	if (pos >= fi.framebuffer_size)
		return 0;
	if ((uint64_t)bytes > fi.framebuffer_size - pos)
		bytes = (long)(fi.framebuffer_size - pos);
	smap_disable();
	kmemcpy(user_buf, base + pos, (size_t)bytes);
	smap_enable();
	return bytes;
}

long fbdev_write(uint64_t pos, const void *user_buf, long bytes)
{
	framebuffer_info_t fi;
	uint8_t *base;

	if (fbdev_current_info(&fi) != 0)
		return -ENODEV;
	base = (uint8_t *)fi.framebuffer_base;
	if (!base || bytes < 0)
		return -EINVAL;
	if (pos >= fi.framebuffer_size)
		return -EINVAL;
	if ((uint64_t)bytes > fi.framebuffer_size - pos)
		bytes = (long)(fi.framebuffer_size - pos);
	smap_disable();
	kmemcpy(base + pos, user_buf, (size_t)bytes);
	smap_enable();
	// Tell the SVGA host about the touched scanlines.
	if (vmsvga2_active() && bytes > 0 && fi.pixels_per_scanline) {
		uint32_t pitch = fi.pixels_per_scanline * fi.bytes_per_pixel;
		uint32_t y0 = (uint32_t)(pos / pitch);
		uint32_t y1 = (uint32_t)((pos + (uint64_t)bytes - 1) / pitch);
		vmsvga2_update_rect(0, y0, fi.horizontal_resolution,
				    y1 - y0 + 1);
	}
	return bytes;
}
