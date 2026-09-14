// LikeOS-64 fbdev core - backend-neutral /dev/fb0 implementation
//
// Bridges the framebuffer device node to whichever display backend owns the
// screen: the VMware SVGA II driver when active, otherwise the boot GOP
// framebuffer.  Exposes the standard fbdev ioctl surface (see uapi/fb.h) and
// the physical range for user mmap of the framebuffer.

#ifndef _KERNEL_DEV_VIDEO_FBDEV_H_
#define _KERNEL_DEV_VIDEO_FBDEV_H_

#include <kernel/uapi/types.h>
#include <kernel/dev/video/fb.h>

struct task;

/* The display driver behind /dev/fb0.
 *
 * Whichever driver owns the screen registers one of these; /dev/fb0 then
 * reports that driver's geometry, maps that driver's memory and routes mode
 * changes and blanking to it.  With none registered the boot framebuffer
 * (the console's own) is what the node shows, with its single mode.  Only
 * get_info and get_phys are required; a NULL optional entry answers the
 * conventional way (no mode changes, blanking pretended, nothing to do on
 * map). */
struct fbdev_backend {
	const char *id; /* fb_fix_screeninfo.id, at most 15 characters */
	int (*get_info)(framebuffer_info_t *out); /* current scanout geometry */
	uint64_t (*get_phys)(uint64_t *size_out); /* base + size for mmap */
	void (*mapped)(void); /* a client mapped the framebuffer */
	/* 0 when the mode can be set, negative errno otherwise. */
	int (*test_mode)(uint32_t w, uint32_t h, uint32_t bpp);
	int (*set_mode)(uint32_t w, uint32_t h, uint32_t bpp);
	int (*blank)(int unblank); /* 1 = display on */
	void (*update_full)(void); /* whole screen changed */
};
/* Register (or, with NULL, withdraw) the backend. */
void fbdev_register_backend(const struct fbdev_backend *b);
const struct fbdev_backend *fbdev_backend(void);

// ioctl entry for /dev/fb0 (FBIOGET_VSCREENINFO, FBIOGET_FSCREENINFO,
// FBIOPUT_VSCREENINFO, FBIOBLANK).  argp is a raw user pointer.
/* Open/close accounting for /dev/fb0.  The last close restores the console,
 * which has been drawn over by whoever mapped the framebuffer. */
/* True while another program has /dev/fb0 open, i.e. owns the display.  The
 * console suppresses its own painting then; see fbdev.c. */
int fbdev_display_owned(void);

void fbdev_opened(void);
void fbdev_closed(void);

int fbdev_ioctl(unsigned long req, void *argp, struct task *cur);

// Physical base of the visible framebuffer and its mappable size.
// Returns 0 when no framebuffer exists.
uint64_t fbdev_get_phys(uint64_t *size_out);

// Validate an mmap request against the framebuffer: returns the physical
// address backing file-offset `offset`, or 0 when [offset, offset+length)
// does not fit the framebuffer.
uint64_t fbdev_mmap_phys(uint64_t offset, uint64_t length);

// pread/pwrite-style access for read()/write() on /dev/fb0.
long fbdev_read(uint64_t pos, void *user_buf, long bytes);
long fbdev_write(uint64_t pos, const void *user_buf, long bytes);

#endif // _KERNEL_DEV_VIDEO_FBDEV_H_
