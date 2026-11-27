// LikeOS pinctrl - Intel community GPIO controllers described by firmware
//
// From Lunar Lake on, firmware may describe the community layout itself
// instead of the OS carrying a table per SoC: the controller (_HID
// INTC105F) has one community per device (memory resource 0), whose
// register offsets are device properties of its _DSD, and its pad groups
// are data-only child nodes of the _DSD, one per group, in hardware
// order:
//
//   device properties
//     intc-gpio-pad-ownership-offset                 PAD_OWN
//     intc-gpio-pad-configuration-lock-offset        PADCFGLOCK
//     intc-gpio-host-software-pad-ownership-offset   HOSTSW_OWN
//     intc-gpio-gpi-interrupt-status-offset          GPI_IS
//     intc-gpio-gpi-interrupt-enable-offset          GPI_IE
//   per group node
//     intc-gpio-group-name    "GPP_A" (pins are named <name>_<n>, '-'
//                             becoming '_')
//     intc-gpio-pad-count     pads in the group
//
// The groups' GPI_IS register is their position in the list, their pins
// follow each other from 0, and their GPIO numbers are their pin numbers.
// Everything else -- revision, features, PADBAR, interrupts, suspend --
// is the core's (pinctrl-intel.c), which also checks the layout against
// the register window before touching anything.
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) 2021-2023 Intel Corporation

#include <kernel/dev/gpio/pinctrl/intel/pinctrl-intel.h>
#include <kernel/hal/acpi_dev.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

#define INTEL_PLATFORM_NAME_LEN 24 /* a group name */
#define INTEL_PLATFORM_PIN_NAME_LEN 32 /* "<group>_<n>" */
#define INTEL_PLATFORM_MAX_PINS 1024

/* Everything one firmware-described controller needs, in one place. */
struct intel_platform_data {
	struct intel_pinctrl_soc_data soc; /* first: freed through it */
	struct intel_community community;
	struct intel_padgroup *gpps;
	struct pinctrl_pin_desc *pins;
	char *names; /* npins * INTEL_PLATFORM_PIN_NAME_LEN */
};

void intel_platform_free_soc_data(struct intel_pinctrl_soc_data *data)
{
	struct intel_platform_data *pd;

	if (!data)
		return;
	pd = container_of(data, struct intel_platform_data, soc);
	kfree(pd->gpps);
	kfree(pd->pins);
	kfree(pd->names);
	kfree(pd);
}

static int intel_platform_read_offset(void *handle, const char *prop,
				      unsigned int *out)
{
	uint32_t v;
	int rc = acpi_dev_get_property_u32(handle, prop, &v);

	if (rc)
		return rc;
	*out = v;
	return 0;
}

/* "<name>_<i>" with every '-' of the name a '_'. */
static void intel_platform_pin_name(char *dst, const char *group,
				    unsigned int i)
{
	ksnprintf(dst, INTEL_PLATFORM_PIN_NAME_LEN, "%s_%u", group, i);
	for (char *p = dst; *p; p++)
		if (*p == '-')
			*p = '_';
}

int intel_platform_prepare_soc_data(void *handle,
				    struct intel_pinctrl_soc_data **out)
{
	struct intel_platform_data *pd;
	struct intel_community *c;
	struct acpi_dsd_nodes *nodes;
	char (*gnames)[INTEL_PLATFORM_NAME_LEN] = NULL;
	unsigned int npins = 0;
	int rc, ngpps;

	might_sleep();
	if (!handle || !out)
		return -EINVAL;
	*out = NULL;
	pd = kcalloc(1, sizeof(*pd));
	nodes = kcalloc(1, sizeof(*nodes));
	if (!pd || !nodes) {
		kfree(pd);
		kfree(nodes);
		return -ENOMEM;
	}
	/* One community per device node in this version of the
	 * description. */
	c = &pd->community;
	c->barno = 0;
	c->pin_base = 0;
	rc = intel_platform_read_offset(handle, "intc-gpio-pad-ownership-offset",
					&c->padown_offset);
	if (!rc)
		rc = intel_platform_read_offset(
			handle, "intc-gpio-pad-configuration-lock-offset",
			&c->padcfglock_offset);
	if (!rc)
		rc = intel_platform_read_offset(
			handle, "intc-gpio-host-software-pad-ownership-offset",
			&c->hostown_offset);
	if (!rc)
		rc = intel_platform_read_offset(
			handle, "intc-gpio-gpi-interrupt-status-offset",
			&c->is_offset);
	if (!rc)
		rc = intel_platform_read_offset(
			handle, "intc-gpio-gpi-interrupt-enable-offset",
			&c->ie_offset);
	if (rc)
		goto out;

	ngpps = acpi_dev_get_data_nodes(handle, nodes);
	if (ngpps <= 0) {
		rc = ngpps ? ngpps : -ENODEV;
		goto out;
	}
	pd->gpps = kcalloc((size_t)ngpps, sizeof(*pd->gpps));
	gnames = kcalloc((size_t)ngpps, sizeof(*gnames));
	if (!pd->gpps || !gnames) {
		rc = -ENOMEM;
		goto out;
	}
	for (int i = 0; i < ngpps; i++) {
		const struct acpi_value *data = nodes->node[i].data;
		struct intel_padgroup *gpp = &pd->gpps[i];
		uint32_t size;

		rc = acpi_dsd_data_get_string(data, "intc-gpio-group-name",
					      gnames[i], sizeof(gnames[i]));
		if (!rc)
			rc = acpi_dsd_data_get_u32(data, "intc-gpio-pad-count",
						   &size);
		if (rc)
			goto out;
		if (size == 0 || size > INTEL_PINCTRL_MAX_GPP_SIZE ||
		    npins + size > INTEL_PLATFORM_MAX_PINS) {
			rc = -EINVAL;
			goto out;
		}
		gpp->reg_num = (unsigned int)i;
		gpp->base = npins;
		gpp->size = size;
		gpp->gpio_base = INTEL_GPIO_BASE_MATCH;
		npins += size;
	}
	pd->pins = kcalloc(npins, sizeof(*pd->pins));
	pd->names = kcalloc(npins, INTEL_PLATFORM_PIN_NAME_LEN);
	if (!pd->pins || !pd->names) {
		rc = -ENOMEM;
		goto out;
	}
	for (int i = 0; i < ngpps; i++) {
		const struct intel_padgroup *gpp = &pd->gpps[i];

		for (unsigned int k = 0; k < gpp->size; k++) {
			unsigned int pin = gpp->base + k;
			char *name = pd->names +
				     (size_t)pin * INTEL_PLATFORM_PIN_NAME_LEN;

			intel_platform_pin_name(name, gnames[i], k);
			pd->pins[pin].number = pin;
			pd->pins[pin].name = name;
		}
	}
	c->gpps = pd->gpps;
	c->ngpps = (size_t)ngpps;
	c->npins = npins;
	pd->soc.communities = c;
	pd->soc.ncommunities = 1;
	pd->soc.pins = pd->pins;
	pd->soc.npins = npins;
	*out = &pd->soc;
	rc = 0;
out:
	kfree(gnames);
	acpi_dev_free_data_nodes(nodes);
	kfree(nodes);
	if (rc)
		intel_platform_free_soc_data(&pd->soc);
	return rc;
}

void intel_platform_pinctrl_init(void)
{
	static const struct acpi_device_id ids[] = {
		{ "INTC105F", 0 },
		{ "", 0 },
	};
	struct acpi_device *devs[8];
	int n;

	might_sleep();
	if (!acpi_dev_ready())
		return;
	n = acpi_dev_collect_matches(ids, 1, devs, 8);
	for (int i = 0; i < n; i++) {
		struct intel_pinctrl_soc_data *soc;
		int rc;

		if (gpiochip_find_by_acpi_handle(devs[i]->handle)) {
			acpi_dev_put(devs[i]);
			continue;
		}
		rc = intel_platform_prepare_soc_data(devs[i]->handle, &soc);
		if (rc) {
			kprintf("intel-pinctrl: %s: no usable layout in _DSD (%d), left alone\n",
				devs[i]->path, rc);
		} else if (intel_pinctrl_probe(devs[i], soc, soc) != 0) {
			intel_platform_free_soc_data(soc);
		}
		acpi_dev_put(devs[i]);
	}
}
