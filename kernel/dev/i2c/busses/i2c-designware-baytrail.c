// LikeOS I2C - DesignWare: the Bay Trail / Cherry Trail PMIC bus
//
// On Bay Trail and Cherry Trail one I2C controller is shared with the
// SoC's power unit (P-Unit), which talks to the PMIC behind the OS's
// back.  Firmware marks that controller with an _SEM method returning
// non-zero.  Every access of the OS to such a bus is bracketed by the
// P-Unit's PMIC bus semaphore, taken and given back over the IOSF
// sideband mailbox (kernel/hal/iosf_mbi.c): the controller's
// acquire_lock/release_lock hooks, which the DesignWare code calls around
// each transfer and every register access outside one.  The controller is
// never runtime-suspended (the P-Unit expects it powered).
//
// Without the mailbox -- or with a P-Unit that does not answer the first
// request -- the bus cannot be shared safely: the probe fails with an
// error other than -ENODEV and the glue does not register the adapter
// (i2c_dw_probe_lock_support).
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) 2014 Intel Corporation

#include <kernel/dev/i2c/busses/i2c-designware-core.h>
#include <kernel/dev/i2c/i2c-core.h>
#include <kernel/hal/acpi_dev.h>
#include <kernel/hal/iosf_mbi.h>
#include <kernel/ke/syscall.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

static int baytrail_i2c_acquire(struct dw_i2c_dev *dev)
{
	(void)dev;
	might_sleep();
	return iosf_mbi_block_punit_i2c_access();
}

static void baytrail_i2c_release(struct dw_i2c_dev *dev)
{
	(void)dev;
	iosf_mbi_unblock_punit_i2c_access();
}

int i2c_dw_baytrail_probe_lock_support(struct dw_i2c_dev *dev)
{
	uint64_t shared_host = 0;
	int ret;

	if (!dev || !dev->acpi_dev)
		return -ENODEV;
	if (!acpi_has_method(dev->acpi_dev->handle, "_SEM"))
		return -ENODEV;
	if (acpi_evaluate_integer(dev->acpi_dev->handle, "_SEM", NULL, 0,
				  &shared_host))
		return -ENODEV;
	if (!shared_host)
		return -ENODEV;

	/* From here the bus is shared with the P-Unit: either the
	 * arbitration works or the adapter must not be used. */
	dev->needs_arbitration = 1;
	(void)iosf_mbi_init();
	if (!iosf_mbi_punit_semaphore_available())
		return -EPROBE_DEFER;

	/* One round trip proves the P-Unit answers; a dead mailbox would
	 * otherwise only show up in the first transfer. */
	ret = iosf_mbi_block_punit_i2c_access();
	if (ret)
		return ret == -ETIMEDOUT ? -ETIMEDOUT : -EIO;
	iosf_mbi_unblock_punit_i2c_access();

	kprintf("%s: I2C bus managed by PUNIT\n", dev->name);
	dev->acquire_lock = baytrail_i2c_acquire;
	dev->release_lock = baytrail_i2c_release;
	dev->shared_with_punit = 1;
	return 0;
}

long i2c_dw_baytrail_show(struct dw_i2c_dev *dev, char *buf, long cap,
			  long pos)
{
	struct iosf_mbi_stats s;
	int held;

	if (!dev->shared_with_punit)
		return pos;
	iosf_mbi_get_stats(&s);
	held = iosf_mbi_punit_semaphore_held();
	return i2c_show_printf(buf, cap, pos,
			       "  dw: punit semaphore %s acquires %lu nested %lu releases %lu timeouts %lu errors %lu max-wait %lu us max-hold %lu us deep-cstates %s\n",
			       held < 0 ? "unreadable" :
			       held	? "held" :
					  "free",
			       s.acquires, s.nested, s.releases, s.timeouts,
			       s.errors, (unsigned long)s.max_wait_us,
			       (unsigned long)s.max_hold_us,
			       iosf_mbi_deep_cstates_allowed() ? "allowed" :
								  "blocked");
}
