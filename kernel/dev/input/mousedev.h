// LikeOS input - the console pointer bridge for touchpads and HID mice
//
// The console's pointer, its scrollback wheel and terminal mouse reporting,
// and every user-space reader of the merged mouse (/dev/input/event1) are
// fed through mouse_inject_usb_movement() (mouse.c).  PS/2, the USB boot
// mouse and VMware's absolute pointer go that way; the input devices the
// HID core creates (an I2C touchpad, the mouse collection next to it) do
// not -- they are evdev devices of their own, meant for libinput.  Until a
// user-space client opens such a device, this bridge listens to it in the
// kernel and turns what it reports into relative motion, buttons and wheel
// steps for the merged mouse:
//
//   touchpads   one finger moves the pointer: each packet moves it by the
//               distance the finger covered over the last two packets,
//               scaled so that the X span of the pad is MOUSEDEV_TP_UNITS
//               units (the same scale on both axes, the fraction carried
//               from packet to packet), from the third packet of a touch
//               on; a short tap (< 200 ms, hardly moved) clicks the left
//               button, a
//               two-finger tap the right one; two fingers moving up/down
//               turn the wheel; the clickpad's button is the left button,
//               the right one when two fingers rest on the pad as it is
//               pressed;
//   mice        relative motion, wheel and the three buttons, as they come.
//
// The bridge keeps each device open in the kernel from its registration
// on, so the HID driver keeps it reporting (hid-multitouch switches a
// touchpad to "report nothing" when its last user closes).  As soon as a
// user-space client has the device's event node open, the bridge stops
// injecting (that client -- libinput in the X server -- drives the pointer
// now, and the two would move it twice) and releases any button it held;
// when the last client closes, it takes over again.
//
// It also reports the reports of I2C touchpads to the PS/2 mouse driver,
// which prefers such a touchpad over a PS/2 emulation of it on the
// auxiliary port (see struct mousedev_ps2_pref below).
//
// Copyright (C) 2026 The LikeOS Project

#ifndef _KERNEL_DEV_INPUT_MOUSEDEV_H_
#define _KERNEL_DEV_INPUT_MOUSEDEV_H_

#include <kernel/uapi/types.h>

struct input_dev;

/* Units a two-packet movement over the whole X span of a touchpad moves the
 * pointer by (before the mouse layer's sensitivity); a steady movement
 * covers each stretch twice, so the pad's span is about twice this. */
#define MOUSEDEV_TP_UNITS 256
/* A touch shorter than this is a tap. */
#define MOUSEDEV_TAP_TIME_MS 200
/* Units of two-finger travel per wheel step (1/32 of the pad's span at a
 * steady movement). */
#define MOUSEDEV_SCROLL_UNITS 16

/* Register the bridge with the input core (it then attaches to every
 * touchpad and relative mouse that registers).  Once, process context,
 * with the work queues up. */
void mousedev_init(void);
/* Detach from every device (buttons the bridge held are released, the
 * devices it kept open are closed) and unregister.  Process context. */
void mousedev_exit(void);

/* What the bridge makes of a device (from its capabilities only). */
#define MOUSEDEV_KIND_NONE 0
#define MOUSEDEV_KIND_TOUCHPAD 1 // ABS_X/Y, BTN_TOUCH, BTN_TOOL_FINGER, not direct
#define MOUSEDEV_KIND_MOUSE 2 // REL_X/Y and BTN_LEFT
int mousedev_device_kind(const struct input_dev *dev);
/* An I2C touchpad in the sense of the PS/2 preference: a pointer device
 * (INPUT_PROP_POINTER) with multitouch slots (ABS_MT_SLOT) on BUS_I2C. */
int mousedev_is_i2c_touchpad(const struct input_dev *dev);

// ---------------------------------------------------------------------------
// PS/2 preference
// ---------------------------------------------------------------------------
//
// A notebook with an I2C touchpad often also has a PS/2 "mouse" on the
// i8042's auxiliary port: the firmware's emulation of the same touchpad
// (used before an operating system drives it over I2C, a "Generic Mouse")
// -- or a real second pointing device, a TrackPoint.  The emulation must
// not move the pointer a second time; the TrackPoint must keep working.
// What sits on the port is known from boot, where the PS/2 driver
// identified it (reset, device ID, the TrackPoint's Read Secondary ID); no
// packet is ever judged by its timing or contents.  The PS/2 driver keeps
// one of these and asks it (under its own lock) whenever an I2C touchpad
// reports:
//
//   - a TrackPoint (pointing stick) is never disabled;
//   - nothing answered at boot: nothing to do;
//   - any other PS/2 device is given up -- the auxiliary port disabled --
//     once an I2C touchpad has delivered its first real report (a finger
//     on it or its button down), i.e. it works.

struct mousedev_ps2_pref {
	int aux_present; // a PS/2 pointing device answered at boot
	int aux_trackpoint; // ... and identified as a TrackPoint
	int disabled; // the verdict was given
};

#define MOUSEDEV_PS2_PASS 0 // keep the port
#define MOUSEDEV_PS2_DISABLE 2 // disable the port now

void mousedev_ps2_pref_init(struct mousedev_ps2_pref *p, int aux_present,
			    int aux_trackpoint);
/* An I2C touchpad reported (contact: a finger on it or its button down).
 * MOUSEDEV_PS2_DISABLE (once) when the port should be disabled now, else
 * MOUSEDEV_PS2_PASS. */
int mousedev_ps2_pref_touchpad(struct mousedev_ps2_pref *p, int contact);

#endif // _KERNEL_DEV_INPUT_MOUSEDEV_H_
