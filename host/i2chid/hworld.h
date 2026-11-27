/* HID-world services of the I2C-HID harness (hworld.c).  HID world only.
 *
 * Copyright (C) 2026 The LikeOS Project */
#ifndef HOST_I2CHID_HWORLD_H
#define HOST_I2CHID_HWORLD_H

#include <kernel/uapi/types.h>

/* The thread that runs the simulated work queue. */
void hw_wq_start(void);
void hw_wq_stop(void);

/* Every mouse_inject_usb_movement() call. */
#define HW_MAX_INJ 8192
struct hw_injection {
	int dx, dy;
	uint8_t buttons;
	int8_t wheel;
};
/* Copy up to `max' (NULL: just count); returns how many there are. */
int hw_injections(struct hw_injection *out, int max);
void hw_injections_clear(void);

/* The PS/2 preference as the mouse driver would keep it: what answered on
 * the auxiliary port at boot. */
void hw_pref_reset(int aux_present, int aux_trackpoint);
/* Verdicts to disable the port so far (and the touchpad's name). */
int hw_pref_state(int *reports, char *name, int cap);

/* From the input harness. */
void test_thread_done(void);

#endif
