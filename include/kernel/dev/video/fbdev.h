// LikeOS-64 fbdev core - backend-neutral /dev/fb0 implementation
//
// Bridges the framebuffer device node to whichever display backend owns the
// screen: the VMware SVGA II driver when active, otherwise the boot GOP
// framebuffer.  Exposes the standard fbdev ioctl surface (see uapi/fb.h) and
// the physical range for user mmap of the framebuffer.

#ifndef _KERNEL_DEV_VIDEO_FBDEV_H_
#define _KERNEL_DEV_VIDEO_FBDEV_H_

#include <kernel/uapi/types.h>

struct task;

// ioctl entry for /dev/fb0 (FBIOGET_VSCREENINFO, FBIOGET_FSCREENINFO,
// FBIOPUT_VSCREENINFO, FBIOBLANK).  argp is a raw user pointer.
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
