// LikeOS GPIO - internals shared by the ACPI glue (gpiolib-acpi.c) and
// its per-machine quirks (gpiolib-acpi-quirks.c)
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) 2012, 2019 Intel Corporation

#ifndef KERNEL_DEV_GPIO_GPIOLIB_ACPI_H
#define KERNEL_DEV_GPIO_GPIOLIB_ACPI_H

#include <kernel/uapi/types.h>

struct acpi_device;

enum acpi_gpio_ignore_list {
	ACPI_GPIO_IGNORE_WAKE,
	ACPI_GPIO_IGNORE_INTERRUPT,
};

/* Run ActiveBoth _AEI events once at start-up when their line is
 * asserted: 1 unless a quirk or the command line says 0. */
int acpi_gpio_need_run_edge_events_on_boot(void);
/* Is a list in force at all (cheap: names need not be made otherwise). */
int acpi_gpio_ignore_list_active(enum acpi_gpio_ignore_list list);
/* Is "<controller>@<pin>" on the list (`controller' an ACPI device name,
 * acpi_gpio_dev_name). */
int acpi_gpio_in_ignore_list(enum acpi_gpio_ignore_list list,
			     const char *controller, unsigned int pin);
/* The ACPI device name of `adev' (which has a _HID): "<_HID>:<instance>",
 * numbered as the ACPI scan numbers (acpi_dev_name).  0 or -errno.  May
 * sleep (the first name walks the namespace). */
int acpi_gpio_dev_name(const struct acpi_device *adev, char *buf, size_t len);

#endif /* KERNEL_DEV_GPIO_GPIOLIB_ACPI_H */
