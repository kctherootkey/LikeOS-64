// LikeOS pinctrl - probe of the GPIO / pin controller drivers
//
// The GPIO controllers of x86 platforms are ACPI devices, enumerated from
// the namespace by their hardware ids: Intel's community controllers from
// Sunrise Point on (a table per SoC, or the layout from the firmware's
// _DSD), Lynx Point / Wildcat Point, Bay Trail and Cherry View with their
// own designs, and AMD's FCH GPIO block.  This runs every driver's probe
// once, after ACPI, PCI and the interrupt core are up and interrupts are
// on, and before the drivers whose devices signal through GPIO
// interrupts (I2C touchpads) look for their lines.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/gpio/gpiolib.h>
#include <kernel/dev/gpio/pinctrl/intel/pinctrl-intel.h>
#include <kernel/hal/acpi_dev.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

void amd_pinctrl_init(void);
void amd_pinctrl_suspend_all(int hibernate);
void amd_pinctrl_resume_all(void);

void gpio_pinctrl_init(void)
{
	static int done;

	might_sleep();
	if (done)
		return;
	done = 1;
	if (!acpi_dev_ready()) {
		kprintf("gpio: no ACPI namespace, no GPIO controllers\n");
		return;
	}
	intel_pinctrl_init();
	intel_platform_pinctrl_init();
	lp_gpio_init();
	byt_pinctrl_init();
	chv_pinctrl_init();
	amd_pinctrl_init();
}

void gpio_pinctrl_suspend_all(void)
{
	might_sleep();
	intel_pinctrl_suspend_all();
	byt_pinctrl_suspend_all();
	chv_pinctrl_suspend_all();
	amd_pinctrl_suspend_all(0);
	/* Lynx Point keeps nothing over sleep: it is set up again on
	 * resume. */
}

void gpio_pinctrl_resume_all(void)
{
	might_sleep();
	/* The reverse order of suspend. */
	amd_pinctrl_resume_all();
	chv_pinctrl_resume_all();
	byt_pinctrl_resume_all();
	lp_gpio_resume_all();
	intel_pinctrl_resume_all();
}
