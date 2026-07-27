// fbtest - /dev/fb0 exerciser in the style of X.org's fbdev driver.
//
// Runs the same device sequence xf86-video-fbdev/fbdevhw performs: open the
// node, query fixed and variable screen info, re-program the current mode,
// unblank, probe the unsupported ioctls it would fall back from, exercise
// the read/write/lseek file interface, then mmap the framebuffer, paint the
// whole screen blue, and finally unmap, restore the mode and close.
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/fb.h>

static int failures = 0;

static void step(const char *name, int ok)
{
	printf("fbtest: %-34s %s", name, ok ? "OK" : "FAIL");
	if (!ok) {
		printf(" (errno=%d %s)", errno, strerror(errno));
		failures++;
	}
	printf("\n");
}

int main(void)
{
	struct fb_var_screeninfo var, saved;
	struct fb_fix_screeninfo fix;

	// fbdevHW: open the device node
	int fd = open("/dev/fb0", O_RDWR);
	step("open /dev/fb0 O_RDWR", fd >= 0);
	if (fd < 0)
		return 1;

	// fbdevHWGetFix analogue
	memset(&fix, 0, sizeof(fix));
	int r = ioctl(fd, FBIOGET_FSCREENINFO, &fix);
	step("FBIOGET_FSCREENINFO", r == 0);
	if (r == 0) {
		printf("fbtest:   id=\"%.16s\" smem=%u bytes line_length=%u\n",
		       fix.id, fix.smem_len, fix.line_length);
		step("fix: packed-pixels truecolor",
		     fix.type == FB_TYPE_PACKED_PIXELS &&
			     fix.visual == FB_VISUAL_TRUECOLOR &&
			     fix.line_length > 0);
	}

	// fbdevHWSave analogue: remember the mode to restore on exit
	memset(&var, 0, sizeof(var));
	r = ioctl(fd, FBIOGET_VSCREENINFO, &var);
	step("FBIOGET_VSCREENINFO", r == 0);
	if (r != 0) {
		close(fd);
		return failures;
	}
	saved = var;
	printf("fbtest:   mode %ux%u-%u pitch=%u (R%u/%u G%u/%u B%u/%u)\n",
	       var.xres, var.yres, var.bits_per_pixel, fix.line_length,
	       var.red.offset, var.red.length, var.green.offset,
	       var.green.length, var.blue.offset, var.blue.length);
	step("var: 16 or 32 bpp",
	     var.bits_per_pixel == 32 || var.bits_per_pixel == 16);

	// fbdevHWModeInit analogue: program the (unchanged) current mode
	var.activate = FB_ACTIVATE_NOW;
	r = ioctl(fd, FBIOPUT_VSCREENINFO, &var);
	step("FBIOPUT_VSCREENINFO (current mode)", r == 0);

	// fbdevHWDPMSSet analogue
	r = ioctl(fd, FBIOBLANK, (void *)(long)FB_BLANK_UNBLANK);
	step("FBIOBLANK UNBLANK", r == 0);

	// X probes these and falls back when the driver lacks them:
	// truecolor fbdev has no cmap or panning here.
	errno = 0;
	r = ioctl(fd, FBIOGETCMAP, &var);
	step("FBIOGETCMAP rejected (EINVAL)", r < 0 && errno == EINVAL);
	errno = 0;
	r = ioctl(fd, FBIOPAN_DISPLAY, &var);
	step("FBIOPAN_DISPLAY rejected (EINVAL)", r < 0 && errno == EINVAL);

	// Linear file interface: read a scanline, write it back, size probe.
	size_t row = fix.line_length;
	unsigned char *rowbuf = malloc(row);
	if (rowbuf) {
		long o = lseek(fd, 0, SEEK_SET);
		long got = read(fd, rowbuf, row);
		step("read first scanline", o == 0 && got == (long)row);
		o = lseek(fd, 0, SEEK_SET);
		long put = write(fd, rowbuf, row);
		step("write first scanline back", o == 0 && put == (long)row);
		long end = lseek(fd, 0, SEEK_END);
		step("lseek SEEK_END >= visible size",
		     end >= (long)fix.line_length * (long)var.yres);
		lseek(fd, 0, SEEK_SET);
		free(rowbuf);
	} else {
		step("alloc scanline buffer", 0);
	}

	// The X.org way to the pixels: map the framebuffer shared.
	size_t maplen = (size_t)fix.line_length * var.yres;
	void *fb = mmap(NULL, maplen, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
			0);
	step("mmap framebuffer MAP_SHARED", fb != MAP_FAILED);

	if (fb != MAP_FAILED) {
		// Fill the visible screen blue, honoring the pitch.
		for (uint32_t y = 0; y < var.yres; y++) {
			unsigned char *line =
				(unsigned char *)fb + (size_t)y * fix.line_length;
			if (var.bits_per_pixel == 32) {
				uint32_t *px = (uint32_t *)line;
				for (uint32_t x = 0; x < var.xres; x++)
					px[x] = 0x000000FFu; // XRGB8888 blue
			} else {
				uint16_t *px = (uint16_t *)line;
				for (uint32_t x = 0; x < var.xres; x++)
					px[x] = 0x001F; // RGB565 blue
			}
		}
		// Hold the blue frame BEFORE printing anything: console output
		// scrolls, and a scroll repaints the full frame from the
		// console's backing store, wiping the fill instantly.
		sleep(3);
		step("fill screen blue via mapping", 1);

		r = munmap(fb, maplen);
		step("munmap framebuffer", r == 0);
	}

	// fbdevHWRestore analogue + close
	saved.activate = FB_ACTIVATE_NOW;
	r = ioctl(fd, FBIOPUT_VSCREENINFO, &saved);
	step("FBIOPUT_VSCREENINFO (restore)", r == 0);
	r = close(fd);
	step("close /dev/fb0", r == 0);

	printf("fbtest: %s (%d failure%s)\n",
	       failures ? "FAILED" : "all tests passed", failures,
	       failures == 1 ? "" : "s");
	printf("fbtest: note: console text reappears on the next redraw\n");
	return failures;
}
