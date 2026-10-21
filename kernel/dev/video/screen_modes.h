// LikeOS - The standard list of screen sizes
// Shared by the UEFI bootloader, the SVGA boot console and the vmwgfx
// display driver, so that all three agree on what can be offered.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef LIKEOS_SCREEN_MODES_H
#define LIKEOS_SCREEN_MODES_H

/* Largest first.  The VESA DMT sizes (the table connectors without an EDID
 * get their modes from, kernel/dev/video/gpu/drm/drm_modes.c), less the
 * text-mode sizes below 640x480, together with the common sizes above
 * 1920x1200 that panels and monitors use but DMT does not list: 2560x1440,
 * 3840x2160 and the rest of the CEA/CVT sizes.  Where a size can actually
 * be shown is for the user of the list to decide. */
static const struct {
	unsigned int width;
	unsigned int height;
} screen_mode_list[] __attribute__((unused)) = {
	{ 7680, 4320 }, { 5120, 2880 }, { 4096, 2160 }, { 3840, 2400 },
	{ 3840, 2160 }, { 3440, 1440 }, { 3200, 1800 }, { 2880, 1800 },
	{ 2560, 1600 }, { 2560, 1440 }, { 2560, 1080 }, { 2048, 1152 },
	{ 1920, 1440 }, { 1920, 1200 }, { 1920, 1080 }, { 1856, 1392 },
	{ 1792, 1344 }, { 1680, 1050 }, { 1600, 1200 }, { 1600, 900 },
	{ 1440, 900 },	{ 1400, 1050 }, { 1366, 768 },	{ 1360, 768 },
	{ 1280, 1024 }, { 1280, 960 },	{ 1280, 800 },	{ 1280, 768 },
	{ 1280, 720 },	{ 1152, 864 },	{ 1024, 768 },	{ 848, 480 },
	{ 800, 600 },	{ 640, 480 },
};

#define SCREEN_MODE_COUNT (sizeof(screen_mode_list) / sizeof(screen_mode_list[0]))

/* The size a machine under a hypervisor comes up in when nothing was saved:
 * the smallest size a virtual machine's window is expected to hold. */
#define SCREEN_VM_WIDTH 1280U
#define SCREEN_VM_HEIGHT 800U

#endif
