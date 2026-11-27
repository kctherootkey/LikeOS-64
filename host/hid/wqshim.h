/* The simulated work queue of the HID harness (wqshim.c).  Kernel-side
 * header: compiled with the prelude.
 *
 * Copyright (C) 2026 The LikeOS Project */
#ifndef HOST_HID_WQSHIM_H
#define HOST_HID_WQSHIM_H

/* Run every queued item, and every delayed item whose deadline the
 * harness clock has passed, once; the number that ran. */
int test_wq_run(void);
/* test_wq_run() until nothing is due any more. */
void test_wq_run_until_idle(void);
/* Items queued or waiting for their deadline. */
int test_wq_pending(void);
/* Items run so far. */
unsigned long test_wq_runs(void);

#endif
