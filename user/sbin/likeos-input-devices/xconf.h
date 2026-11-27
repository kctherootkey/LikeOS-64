// likeos-input-devices - the X server configuration for this machine's
// touchpads: finding them (through libudev) and writing the InputDevice
// sections that hand each one to the libinput driver.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef LIKEOS_INPUT_DEVICES_XCONF_H
#define LIKEOS_INPUT_DEVICES_XCONF_H

#include <stdbool.h>
#include <stddef.h>

struct udev;

// The file the X server reads from its configuration directory.  Files
// there are read before /etc/X11/xorg.conf and its siblings.
#define LID_DEFAULT_OUTPUT "/etc/X11/xorg.conf.d/50-likeos-touchpads.conf"
// Settings that override the defaults below (see lid_options_parse).
#define LID_DEFAULT_SETTINGS "/etc/likeos/touchpad.conf"

#define LID_NAME_MAX 96

struct lid_touchpad {
	int event;                 // N of /dev/input/eventN
	char name[LID_NAME_MAX];   // the kernel's name, made safe for the file
	unsigned int bustype, vendor, product;
	bool buttonpad;            // the whole surface is the button (clickpad)
	bool multifinger;          // can tell two fingers from one
};

// Tri-state switches: -1 = leave to the driver.
struct lid_options {
	int tapping;
	int tapping_drag;
	int tapping_drag_lock;
	int natural_scrolling;
	int disable_while_typing;
	int left_handed;
	int middle_emulation;
	int horizontal_scrolling;
	char scroll_method[16];   // "twofinger", "edge", "none"; "" = by device
	char click_method[16];    // "clickfinger", "buttonareas", "none"
	char accel_profile[16];   // "adaptive", "flat"
	char accel_speed[16];     // "-1.0" .. "1.0", "" = driver default
};

// The desktop defaults: tap to click and tap-and-drag, two-finger scrolling
// (edge scrolling on a touchpad that cannot tell two fingers apart),
// two-finger click for the right button on clickpads, no natural scrolling,
// no pointer motion while typing, adaptive acceleration.
void lid_options_defaults(struct lid_options *o);

// Apply KEY=VALUE lines (# comments, blank lines allowed) over `o'.
// Unknown keys and invalid values are reported into `err' (the first one)
// and skipped.  Returns the number of lines skipped.
int lid_options_parse(struct lid_options *o, const char *text, char *err,
		      size_t errlen);

// The touchpads libudev reports (ID_INPUT_TOUCHPAD event nodes), in node
// order.  0 or a negative errno; *out is malloc'ed (NULL when none).
int lid_collect(struct udev *udev, struct lid_touchpad **out, size_t *count);

// The configuration text for these touchpads; NULL (errno ENOMEM) on
// failure.  With no touchpads there is nothing to write: the caller removes
// the file instead, so a machine without one starts X exactly as before.
char *lid_render(const struct lid_touchpad *tp, size_t count,
		 const struct lid_options *o);

#endif
