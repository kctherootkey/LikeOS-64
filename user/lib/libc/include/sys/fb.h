// LikeOS-64 framebuffer device UAPI - /dev/fb0 ioctl interface
//
// Layout and request numbers follow the de-facto standard fbdev interface
// so existing display servers (X.org fbdev/modesetting) can be ported
// against it unchanged.  Mirrored into the userspace libc include tree;
// keep both copies in sync.

#ifndef _KERNEL_UAPI_FB_H_
#define _KERNEL_UAPI_FB_H_

#ifdef __LIKEOS__
#include <kernel/uapi/types.h>
#else
#include <stdint.h>
#endif

// ioctl request numbers
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOGETCMAP	    0x4604
#define FBIOPUTCMAP	    0x4605
#define FBIOPAN_DISPLAY	    0x4606
#define FBIOBLANK	    0x4611

// fb_fix_screeninfo.type
#define FB_TYPE_PACKED_PIXELS 0

// fb_fix_screeninfo.visual
#define FB_VISUAL_MONO01    0
#define FB_VISUAL_MONO10    1
#define FB_VISUAL_TRUECOLOR 2
#define FB_VISUAL_PSEUDOCOLOR 3
#define FB_VISUAL_DIRECTCOLOR 4

// fb_fix_screeninfo.accel
#define FB_ACCEL_NONE 0

// fb_var_screeninfo.activate
#define FB_ACTIVATE_NOW  0
#define FB_ACTIVATE_TEST 4

// fb_var_screeninfo.vmode
#define FB_VMODE_NONINTERLACED 0

// FBIOBLANK levels
#define FB_BLANK_UNBLANK  0
#define FB_BLANK_NORMAL	  1
#define FB_BLANK_POWERDOWN 4

struct fb_bitfield {
	uint32_t offset; // beginning of bitfield
	uint32_t length; // length of bitfield
	uint32_t msb_right; // != 0: most significant bit is right
};

struct fb_fix_screeninfo {
	char id[16]; // identification string, e.g. "svga2" / "gopfb"
	unsigned long smem_start; // physical start of framebuffer memory
	uint32_t smem_len; // length of framebuffer memory
	uint32_t type; // FB_TYPE_*
	uint32_t type_aux;
	uint32_t visual; // FB_VISUAL_*
	uint16_t xpanstep; // zero if no hardware panning
	uint16_t ypanstep;
	uint16_t ywrapstep;
	uint32_t line_length; // length of a line in bytes (pitch)
	unsigned long mmio_start; // physical start of MMIO region (0 if none)
	uint32_t mmio_len;
	uint32_t accel; // FB_ACCEL_*
	uint16_t capabilities;
	uint16_t reserved[2];
};

struct fb_var_screeninfo {
	uint32_t xres; // visible resolution
	uint32_t yres;
	uint32_t xres_virtual; // virtual resolution
	uint32_t yres_virtual;
	uint32_t xoffset; // offset from virtual to visible
	uint32_t yoffset;
	uint32_t bits_per_pixel;
	uint32_t grayscale; // 0 = color
	struct fb_bitfield red; // bitfields in the pixel value
	struct fb_bitfield green;
	struct fb_bitfield blue;
	struct fb_bitfield transp;
	uint32_t nonstd; // != 0: non-standard pixel format
	uint32_t activate; // FB_ACTIVATE_*
	uint32_t height; // height of picture in mm (0 = unknown)
	uint32_t width; // width of picture in mm (0 = unknown)
	uint32_t accel_flags;
	// Timing values are informational only for virtual hardware.
	uint32_t pixclock; // pixel clock in ps
	uint32_t left_margin;
	uint32_t right_margin;
	uint32_t upper_margin;
	uint32_t lower_margin;
	uint32_t hsync_len;
	uint32_t vsync_len;
	uint32_t sync;
	uint32_t vmode; // FB_VMODE_*
	uint32_t rotate;
	uint32_t colorspace;
	uint32_t reserved[4];
};

#endif // _KERNEL_UAPI_FB_H_
