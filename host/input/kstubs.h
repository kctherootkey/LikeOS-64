/* What the input core harness's kernel services (kstubs.c) offer the test
 * scenarios (tests.c).  Kernel-side header: compiled with the prelude.
 *
 * Copyright (C) 2026 The LikeOS Project */
#ifndef HOST_INPUT_KSTUBS_H
#define HOST_INPUT_KSTUBS_H

#include <kernel/uapi/types.h>
#include <kernel/uapi/devevent.h>

/* The clock timer_get_precise_ns() returns: a fixed value the tests set,
 * or (0) the host's monotonic clock. */
void test_set_clock_ns(uint64_t ns);
#define TEST_REALTIME_OFFSET_NS (1700000000ULL * 1000000000ULL)

/* WARNING lines the kernel code printed (WARN, WARN_ON, ...). */
long test_warning_count(void);
/* Make every sleeping reader of this thread see a pending signal. */
void test_set_signal_pending(int on);

/* Device events published so far (a ring of the last 64). */
struct test_devevent {
	uint32_t type, device_class, flags, major, minor;
	uint64_t device_id;
	uint16_t vendor, product;
	char name[32];
};
unsigned test_devevent_count(void);
int test_devevent_get(unsigned index, struct test_devevent *out);

/* Wake-ups of poll queues, by POLL* key bit. */
unsigned long test_poll_wakes(unsigned key_bit);

/* The keyboard's world (kbstubs.c): whether the caller is privileged
 * (capable()), what the console terminal was typed since the last take (as
 * a string), how many debug dumps the console's hotkeys ran, and how often
 * the console was repainted (a release of its output gate that found the
 * display free). */
void test_set_capable(int on);
int test_console_take(char *out, int cap);
int test_console_dumps(void);
int test_console_redraws(void);
/* The output gate's other signal: a DRM device getting (set) or losing its
 * master, as drm_master_set/drm_master_drop report it.  And how often a
 * take found the display owned (the hardware cursor goes down). */
void test_display_master(int set);
int test_display_takes(void);

/* Release the calling thread's simulated task (end of a thread). */
void test_thread_done(void);
/* Tasks currently parked in sched_schedule(). */
int test_sleepers(void);

#endif
