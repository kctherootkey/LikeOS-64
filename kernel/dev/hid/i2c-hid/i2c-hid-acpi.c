// LikeOS I2C-HID - the ACPI binding
//
// Firmware describes a HID-over-I2C device as an ACPI device with the
// compatible id PNP0C50 (or ACPI0C50) below its I2C controller, with an
// I2cSerialBus resource (address, speed) and an interrupt (GpioInt or
// Interrupt) -- the I2C core turns that into a client named after the
// device ("ELAN073B:00") and resolves the interrupt before probe.  What
// the resources cannot say, the address of the HID descriptor register,
// comes from the device's _DSM: function 1 of the HID-over-I2C UUID
// 3cdff6f7-4267-4555-ad05-b30a3d8938de, revision 1, returns it as an
// integer.  The I2C core has put the device in D0 (_PS0 / its power
// resources) before probe and puts it back in D3 after remove.
//
// Excluded although they carry the id: the Chipone ICN8505 description
// (CHPN0001, not HID compatible) and IDEA5002 (high interrupt load,
// spurious wake-ups).
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) 2012 Benjamin Tissoires, 2012 Ecole Nationale de l'Aviation Civile, 2012 Red Hat, Inc

#include <kernel/dev/hid/i2c-hid/i2c-hid.h>
#include <kernel/dev/hid/core/hid.h>
#include <kernel/dev/i2c/i2c.h>
#include <kernel/hal/acpi_dev.h>
#include <kernel/hal/dmi.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>
#include <kernel/ke/syscall.h> /* errno values */

struct i2c_hid_acpi {
	struct i2chid_ops ops; // first: container of the ops
	struct acpi_device *adev; // a reference
};

static struct i2c_hid_acpi *ops_to_acpi(struct i2chid_ops *ops)
{
	return (struct i2c_hid_acpi *)ops;
}

static const struct acpi_device_id i2c_hid_acpi_blacklist[] = {
	/* The CHPN0001 ACPI device, which describes the Chipone ICN8505
	 * controller, has a _CID of PNP0C50 but is not HID compatible. */
	{ "CHPN0001", 0 },
	/* The IDEA5002 ACPI device causes high interrupt usage and spurious
	 * wakeups from suspend. */
	{ "IDEA5002", 0 },
	{ "", 0 },
};

/* The HID descriptor register from _DSM: the register (0..0xffff) or
 * -ENODEV. */
static int i2c_hid_acpi_get_descriptor(struct acpi_device *adev,
				       const char *dev_name)
{
	/* HID I2C Device: 3cdff6f7-4267-4555-ad05-b30a3d8938de */
	static const char i2c_hid_guid[] =
		"3cdff6f7-4267-4555-ad05-b30a3d8938de";
	struct acpi_value *obj = NULL;
	uint8_t uuid[16];
	uint64_t addr;
	int rc;

	if (acpi_uuid_parse(i2c_hid_guid, uuid))
		return -EINVAL;
	rc = acpi_evaluate_dsm_typed(adev->handle, uuid, 1, 1, NULL,
				     ACPI_VALUE_INTEGER, &obj);
	if (rc || !obj) {
		kprintf("i2c_hid_acpi %s: %s: Error _DSM call to get HID descriptor address failed (%d)\n",
			dev_name, adev->path, rc);
		acpi_value_free(obj);
		return -ENODEV;
	}
	addr = obj->integer;
	acpi_value_free(obj);
	/* The register address is 16 bits; anything wider is a broken
	 * method, and its low half would point anywhere. */
	if (addr > 0xffff) {
		kprintf("i2c_hid_acpi %s: %s: _DSM returned an invalid HID descriptor address %#llx\n",
			dev_name, adev->path, (unsigned long long)addr);
		return -ENODEV;
	}
	return (int)addr;
}

/* HP OmniBook X Flip 14 (board 8D9F), OmniBook 7 Flip 16 (8DA0) and
 * OmniBook X Flip 16 (8DA1): the touchscreen power resource
 * \_SB.PC00.I2C4.PTPL._ON writes the wrong value to the GPIROUTIOXAPIC bit
 * of the touchscreen interrupt pad, disconnecting it from the IO-APIC.
 * The level-triggered interrupt then never deasserts, the controller is
 * read continuously and pen reports cannot come at their native rate.
 * Windows does not run _ON while the resource already reports on.  Here
 * _ON runs at probe (and on every resume), so the routing is re-enabled
 * with the firmware's own helper before the HID reset. */
static const struct dmi_system_id i2c_hid_acpi_irq_route_dmi[] = {
	{
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "HP"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "8D9F"),
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "HP"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "8DA0"),
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "HP"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "8DA1"),
		},
	},
	{ 0 }
};

static int i2c_hid_acpi_restore_irq_route(struct i2chid_ops *ops)
{
	struct i2c_hid_acpi *ihid_acpi = ops_to_acpi(ops);
	struct acpi_value args[2];
	uint64_t pad;
	int rc;

	if (acpi_evaluate_integer(NULL, "\\GPLI", NULL, 0, &pad))
		return 0;
	kmemset(args, 0, sizeof(args));
	args[0].type = ACPI_VALUE_INTEGER;
	args[0].integer = pad;
	args[1].type = ACPI_VALUE_INTEGER;
	args[1].integer = 1;
	rc = acpi_evaluate(NULL, "\\_SB.SGRA", args, 2, NULL);
	if (rc)
		kprintf("i2c_hid_acpi: %s: failed to restore interrupt routing: %d\n",
			ihid_acpi->adev->path, rc);
	/* Never fail probe or resume because of this workaround. */
	return 0;
}

static void i2c_hid_acpi_shutdown_tail(struct i2chid_ops *ops)
{
	struct i2c_hid_acpi *ihid_acpi = ops_to_acpi(ops);

	(void)acpi_device_set_power(ihid_acpi->adev, ACPI_DEV_D3_COLD);
}

static int i2c_hid_acpi_probe(struct i2c_client *client)
{
	struct acpi_device *adev = client->acpi_dev;
	struct i2c_hid_acpi *ihid_acpi;
	uint16_t hid_descriptor_address;
	int ret;

	might_sleep();
	if (!adev)
		return -ENODEV;
	if (acpi_match_device_ids(adev, i2c_hid_acpi_blacklist))
		return -ENODEV;

	ret = i2c_hid_acpi_get_descriptor(adev, client->dev_name);
	if (ret < 0)
		return ret;
	hid_descriptor_address = (uint16_t)ret;

	ihid_acpi = kcalloc(1, sizeof(*ihid_acpi));
	if (!ihid_acpi)
		return -ENOMEM;
	ihid_acpi->adev = acpi_dev_get(adev);
	ihid_acpi->ops.shutdown_tail = i2c_hid_acpi_shutdown_tail;
	if (!kstrcmp(adev->hid, "ELAN2514") &&
	    dmi_first_match(i2c_hid_acpi_irq_route_dmi))
		ihid_acpi->ops.power_up = i2c_hid_acpi_restore_irq_route;

	ret = i2c_hid_core_probe(client, &ihid_acpi->ops,
				 hid_descriptor_address, 0);
	if (ret) {
		acpi_dev_put(ihid_acpi->adev);
		kfree(ihid_acpi);
	}
	return ret;
}

static void i2c_hid_acpi_remove(struct i2c_client *client)
{
	struct i2c_hid_acpi *ihid_acpi;

	/* The ops the probe handed the core are the first member of the
	 * binding's state: fetch them before the core forgets them. */
	ihid_acpi = (struct i2c_hid_acpi *)i2c_hid_core_ops(client);
	i2c_hid_core_remove(client);
	if (ihid_acpi) {
		acpi_dev_put(ihid_acpi->adev);
		kfree(ihid_acpi);
	}
}

static void i2c_hid_acpi_shutdown(struct i2c_client *client)
{
	i2c_hid_core_shutdown(client);
}

static int i2c_hid_acpi_suspend(struct i2c_client *client)
{
	return i2c_hid_core_pm_suspend(client);
}

static int i2c_hid_acpi_resume(struct i2c_client *client)
{
	return i2c_hid_core_pm_resume(client);
}

static const struct acpi_device_id i2c_hid_acpi_match[] = {
	{ "ACPI0C50", 0 },
	{ "PNP0C50", 0 },
	{ "", 0 },
};

static struct i2c_driver i2c_hid_acpi_driver = {
	.name = "i2c_hid_acpi",
	.probe = i2c_hid_acpi_probe,
	.remove = i2c_hid_acpi_remove,
	.shutdown = i2c_hid_acpi_shutdown,
	.suspend = i2c_hid_acpi_suspend,
	.resume = i2c_hid_acpi_resume,
	.show = i2c_hid_core_show,
	.acpi_match_table = i2c_hid_acpi_match,
};

int i2c_hid_acpi_init(void)
{
	int ret;

	might_sleep();
	/* The HID drivers (hid-multitouch, hid-generic) must be there before
	 * the first device is added. */
	hid_core_init();
	ret = i2c_register_driver(&i2c_hid_acpi_driver);
	if (ret)
		kprintf("i2c_hid_acpi: driver registration failed: %d\n", ret);
	return ret;
}

void i2c_hid_acpi_exit(void)
{
	i2c_del_driver(&i2c_hid_acpi_driver);
}
