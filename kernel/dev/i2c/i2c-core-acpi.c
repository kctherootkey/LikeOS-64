// LikeOS I2C - ACPI glue of the core
//
// Firmware describes the devices on an I2C bus as ACPI devices anywhere
// in the namespace (usually, not necessarily, below the controller) whose
// _CRS holds an I2cSerialBus descriptor naming the controller in its
// ResourceSource.  This file
//
//   * scans the namespace once for such devices and keeps the result
//     (device, controller handle, address, 10-bit flag, connection speed,
//     the ACPI instance name "<HID>:<NN>") -- every adapter registration
//     and speed lookup uses the same list;
//   * creates the clients of an adapter (i2c_acpi_register_devices):
//     every device present (or functioning), ready for enumeration (_DEP),
//     not a video device, whose first I2cSerialBus resolves to exactly the
//     adapter's ACPI handle -- except on boards known to describe bogus
//     I2C devices (a DMI list; only the ids known to be right are taken
//     there).  The client is named after the ACPI device
//     instance ("ELAN073B:00"), its device name is "i2c-ELAN073B:00";
//   * finds the bus speed of an adapter: the slowest connection of its
//     devices, with the known firmware fixups (a few devices force 100 or
//     400 kHz);
//   * resolves a client's interrupt at probe time: the first Interrupt /
//     ExtendedIRQ resource (an I/O APIC line) or else the first GpioInt
//     (through gpiolib, which answers -EPROBE_DEFER while the GPIO
//     controller is missing);
//   * powers a client's ACPI device to D0 for probe and back down after;
//   * serves AML accesses through GenericSerialBus operation regions
//     declared under an adapter's ACPI device (send/receive, byte, word,
//     block, multi-byte), so methods of touchpads, batteries and chargers
//     that talk to I2C devices work;
//   * looks up bus recovery GPIOs (_DSD "scl-gpios"/"sda-gpios").
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) 2014 Intel Corporation, 2017 Hans de Goede

/* The ACPICA headers carry unused parameters in their stubs. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "kernel/hal/acpica/include/acpi.h"
#pragma GCC diagnostic pop

#include <kernel/dev/i2c/i2c-core.h>
#include <kernel/dev/gpio/gpiolib.h>
#include <kernel/hal/acpi_dev.h>
#include <kernel/hal/dmi.h>
#include <kernel/ke/irq.h>
#include <kernel/ke/irq_internal.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/delay.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

/* At most this many namespace devices are looked at. */
#define I2C_ACPI_MAX_DEVICES 4096

/* One namespace device with an I2cSerialBus connection. */
struct i2c_acpi_node {
	void *handle;
	void *adapter_handle; /* the controller its resource names */
	uint32_t speed;
	uint16_t addr;
	uint8_t ten_bit;
	uint8_t enumerated; /* a client exists for it */
	char name[ACPI_ID_LEN + 4]; /* "ELAN073B:00" */
	char hid[ACPI_ID_LEN];
};

static DEFINE_MUTEX(g_acpi_scan_lock);
static struct i2c_acpi_node *g_nodes;
static int g_nnodes;
static int g_scanned;

/* These Silead touchscreen controllers only work at 400 kHz; other
 * devices on their bus that the tables list at 100 kHz have been seen to
 * work at 400 kHz, so the bus goes to 400 kHz. */
static const struct acpi_device_id i2c_acpi_force_400khz_device_ids[] = {
	{ "MSSL1680", 0 },
	{ "", 0 },
};

/* These touchpads smooth their reports excessively at 400 kHz, while
 * their tables ask for it; they get 100 kHz. */
static const struct acpi_device_id i2c_acpi_force_100khz_device_ids[] = {
	{ "ASUE140D", 0 },
	{ "DELL0A86", 0 },
	{ "DLL0945", 0 },
	{ "ELAN0678", 0 },
	{ "ELAN06FA", 0 },
	{ "ELAN1300", 0 },
	{ "", 0 },
};

/* x86 boards that shipped with only Android as their factory image
 * usually declare a whole bunch of bogus I2C devices in their ACPI tables
 * (devices that are not there, or are elsewhere, causing GPIO and
 * interrupt conflicts when drivers bind to them).  On these boards no
 * ACPI client is created except for the ids known to be described
 * correctly (codecs, PMICs, the keyboard dock). */
static const struct dmi_system_id i2c_acpi_skip_clients_dmi_ids[] = {
	{
		/* Acer Iconia One 7 B1-750 */
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Insyde"),
			DMI_MATCH(DMI_PRODUCT_NAME, "VESPA2"),
		},
	},
	{
		/* Acer Iconia One 8 A1-840 (non FHD version) */
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Insyde"),
			DMI_MATCH(DMI_PRODUCT_NAME, "BayTrail"),
			/* Above strings are too generic also match BIOS date */
			DMI_MATCH(DMI_BIOS_DATE, "04/01/2014"),
		},
	},
	{
		/* Asus ME176C tablet */
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "ASUSTeK COMPUTER INC."),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "ME176C"),
		},
	},
	{
		/* Asus TF103C transformer 2-in-1 */
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "ASUSTeK COMPUTER INC."),
			DMI_MATCH(DMI_PRODUCT_NAME, "TF103C"),
		},
	},
	{
		/* Lenovo Yoga Book X90F/L */
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Intel Corporation"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "CHERRYVIEW D1 PLATFORM"),
			DMI_EXACT_MATCH(DMI_PRODUCT_VERSION, "YETI-11"),
		},
	},
	{
		/* Lenovo Yoga Tablet 2 1050F/L */
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Intel Corp."),
			DMI_MATCH(DMI_PRODUCT_NAME, "VALLEYVIEW C0 PLATFORM"),
			DMI_MATCH(DMI_BOARD_NAME, "BYT-T FFD8"),
			/* Partial match on beginning of BIOS version */
			DMI_MATCH(DMI_BIOS_VERSION, "BLADE_21"),
		},
	},
	{
		/* Lenovo Yoga Tab 3 Pro X90F */
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Intel Corporation"),
			DMI_MATCH(DMI_PRODUCT_VERSION, "Blade3-10A-001"),
		},
	},
	{
		/* Medion Lifetab S10346 */
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "AMI Corporation"),
			DMI_MATCH(DMI_BOARD_NAME, "Aptio CRB"),
			/* Way too generic, also match on BIOS data */
			DMI_MATCH(DMI_BIOS_DATE, "10/22/2015"),
		},
	},
	{
		/* Nextbook Ares 8 (BYT version)*/
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Insyde"),
			DMI_MATCH(DMI_PRODUCT_NAME, "M890BAP"),
		},
	},
	{
		/* Nextbook Ares 8A (CHT version)*/
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Insyde"),
			DMI_MATCH(DMI_PRODUCT_NAME, "CherryTrail"),
			DMI_MATCH(DMI_BIOS_VERSION, "M882"),
		},
	},
	{
		/* Vexia Edu Atla 10 tablet 5V version */
		.matches = {
			/* Having all 3 of these not set is somewhat unique */
			DMI_MATCH(DMI_SYS_VENDOR, "To be filled by O.E.M."),
			DMI_MATCH(DMI_PRODUCT_NAME, "To be filled by O.E.M."),
			DMI_MATCH(DMI_BOARD_NAME, "To be filled by O.E.M."),
			/* Above strings are too generic, also match on BIOS date */
			DMI_MATCH(DMI_BIOS_DATE, "05/14/2015"),
		},
	},
	{
		/* Vexia Edu Atla 10 tablet 9V version */
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "AMI Corporation"),
			DMI_MATCH(DMI_BOARD_NAME, "Aptio CRB"),
			/* Above strings are too generic, also match on BIOS date */
			DMI_MATCH(DMI_BIOS_DATE, "08/25/2014"),
		},
	},
	{
		/* Whitelabel (sold as various brands) TM800A550L */
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "AMI Corporation"),
			DMI_MATCH(DMI_BOARD_NAME, "Aptio CRB"),
			/* Above strings are too generic, also match on BIOS version */
			DMI_MATCH(DMI_BIOS_VERSION, "ZY-8-BI-PX4S70VTR400-X423B-005-D"),
		},
	},
	{ 0 }
};

static const struct acpi_device_id i2c_acpi_known_good_ids[] = {
	{ "10EC5640", 0 }, /* RealTek ALC5640 audio codec */
	{ "10EC5651", 0 }, /* RealTek ALC5651 audio codec */
	{ "INT33F4", 0 }, /* X-Powers AXP288 PMIC */
	{ "INT33F5", 0 }, /* TI Dollar Cove PMIC */
	{ "INT33FD", 0 }, /* Intel Crystal Cove PMIC */
	{ "INT34D3", 0 }, /* Intel Whiskey Cove PMIC */
	{ "NPCE69A", 0 }, /* Asus Transformer keyboard dock */
	{ "", 0 },
};

/* Skip the ACPI client `adev' on this board (bogus tables)? */
int i2c_acpi_quirk_skip_client_enumeration(const struct acpi_device *adev)
{
	if (!dmi_first_match(i2c_acpi_skip_clients_dmi_ids))
		return 0;
	return acpi_match_device_ids(adev, i2c_acpi_known_good_ids) == NULL;
}

/* ==========================================================================
 * The namespace scan
 * ========================================================================== */

struct scan_entry {
	void *handle;
	char id[ACPI_ID_LEN]; /* _HID, or the first _CID */
};

struct scan_ctx {
	struct scan_entry *e;
	int n, cap;
	int overflow;
};

static int scan_collect_cb(struct acpi_device *adev,
			   const struct acpi_device_id *id, void *arg)
{
	struct scan_ctx *sc = arg;

	(void)id;
	if (sc->n >= sc->cap) {
		sc->overflow = 1;
		return 1;
	}
	sc->e[sc->n].handle = adev->handle;
	i2c_strlcpy(sc->e[sc->n].id,
		    adev->has_hid ? adev->hid :
				    (adev->ncids ? adev->cid[0] : ""),
		    sizeof(sc->e[sc->n].id));
	sc->n++;
	return 0;
}

/* Present or functioning (the enumeration rule). */
static int i2c_acpi_dev_present(const struct acpi_device *adev)
{
	return (adev->sta & (ACPI_DEV_STA_PRESENT | ACPI_DEV_STA_FUNCTIONING)) !=
	       0;
}

/* Devices of the video extension sometimes carry an I2C connection of
 * their own; the display driver owns them. */
static int i2c_acpi_is_video_device(struct acpi_device *adev)
{
	if (acpi_dev_has_id(adev, "LNXVIDEO"))
		return 1;
	return acpi_has_method(adev->handle, "_DOD") ||
	       acpi_has_method(adev->handle, "_DOS");
}

/* Look at one collected device: its first I2cSerialBus, if any. */
static void scan_one(const struct scan_entry *all, int idx,
		     struct i2c_acpi_node *out, int *nout)
{
	const struct scan_entry *se = &all[idx];
	struct acpi_device *adev;
	struct acpi_res_list res;
	const struct acpi_res *r;
	void *adapter;

	if (!acpi_has_method(se->handle, "_CRS"))
		return;
	adev = acpi_dev_from_handle(se->handle);
	if (!adev)
		return;
	/* _CRS of a device that is not there may not even evaluate. */
	if (!i2c_acpi_dev_present(adev) || i2c_acpi_is_video_device(adev)) {
		acpi_dev_put(adev);
		return;
	}
	kmemset(&res, 0, sizeof(res));
	if (acpi_dev_get_resources(adev, &res)) {
		acpi_dev_put(adev);
		return;
	}
	r = acpi_res_find(&res, ACPI_RES_I2C, 0);
	adapter = r ? acpi_get_handle_by_path(se->handle, r->i2c.source) :
		      NULL;
	if (r && adapter) {
		struct i2c_acpi_node *n = &out[(*nout)++];

		n->handle = se->handle;
		n->adapter_handle = adapter;
		n->speed = r->i2c.speed_hz;
		n->addr = r->i2c.address;
		n->ten_bit = r->i2c.ten_bit ? 1 : 0;
		n->enumerated = 0;
		i2c_strlcpy(n->hid, se->id, sizeof(n->hid));
		/* The ACPI device name, numbered as the ACPI scan numbers
		 * (acpi_dev_name): the same name the GPIO quirk lists and
		 * user space quirk tables use. */
		if (acpi_dev_name(se->handle, n->name, sizeof(n->name)) != 0)
			ksnprintf(n->name, sizeof(n->name), "%s:00", se->id);
	} else if (r) {
		kprintf("i2c: %s: I2C controller %s not found\n", adev->path,
			r->i2c.source);
	}
	acpi_dev_free_resource_list(&res);
	acpi_dev_put(adev);
}

/* Build the list once (g_acpi_scan_lock held).  0 or -errno. */
static int i2c_acpi_scan_locked(void)
{
	struct scan_ctx sc;
	struct i2c_acpi_node *nodes;
	int rc, nout = 0;

	if (g_scanned)
		return 0;
	if (!acpi_dev_ready())
		return -ENODEV;
	kmemset(&sc, 0, sizeof(sc));
	sc.cap = I2C_ACPI_MAX_DEVICES;
	sc.e = kcalloc((size_t)sc.cap, sizeof(*sc.e));
	if (!sc.e)
		return -ENOMEM;
	/* All devices with an id, in namespace order; resources are read
	 * after the walk, not inside it. */
	rc = acpi_dev_for_each_match(NULL, 0, scan_collect_cb, &sc);
	if (rc < 0) {
		kfree(sc.e);
		return rc;
	}
	if (sc.overflow)
		kprintf("i2c: more than %d ACPI devices, the rest not scanned\n",
			sc.cap);
	nodes = kcalloc((size_t)(sc.n ? sc.n : 1), sizeof(*nodes));
	if (!nodes) {
		kfree(sc.e);
		return -ENOMEM;
	}
	for (int i = 0; i < sc.n; i++)
		scan_one(sc.e, i, nodes, &nout);
	kfree(sc.e);
	g_nodes = nodes;
	g_nnodes = nout;
	g_scanned = 1;
	return 0;
}

void i2c_acpi_invalidate_cache(void)
{
	mutex_lock(&g_acpi_scan_lock);
	kfree(g_nodes);
	g_nodes = NULL;
	g_nnodes = 0;
	g_scanned = 0;
	mutex_unlock(&g_acpi_scan_lock);
}

/* ==========================================================================
 * Bus speed
 * ========================================================================== */

uint32_t i2c_acpi_find_bus_speed(void *adapter_handle)
{
	uint32_t min_speed = 0xffffffffu, force_speed = 0;

	might_sleep();
	if (!adapter_handle)
		return 0;
	mutex_lock(&g_acpi_scan_lock);
	if (i2c_acpi_scan_locked()) {
		mutex_unlock(&g_acpi_scan_lock);
		return 0;
	}
	for (int i = 0; i < g_nnodes; i++) {
		struct i2c_acpi_node *n = &g_nodes[i];
		struct acpi_device *adev;

		if (n->adapter_handle != adapter_handle)
			continue;
		adev = acpi_dev_from_handle(n->handle);
		if (!adev)
			continue;
		if (i2c_acpi_dev_present(adev) &&
		    acpi_dev_ready_for_enumeration(adev)) {
			if (n->speed <= min_speed)
				min_speed = n->speed;
			if (acpi_match_device_ids(adev,
						  i2c_acpi_force_400khz_device_ids))
				force_speed = I2C_MAX_FAST_MODE_FREQ;
			if (acpi_match_device_ids(adev,
						  i2c_acpi_force_100khz_device_ids))
				force_speed = I2C_MAX_STANDARD_MODE_FREQ;
		}
		acpi_dev_put(adev);
	}
	mutex_unlock(&g_acpi_scan_lock);

	if (force_speed) {
		if (force_speed != min_speed)
			kprintf("i2c: [Firmware Bug]: DSDT uses known not-working I2C bus speed %u, forcing it to %u\n",
				min_speed, force_speed);
		return force_speed;
	}
	return min_speed != 0xffffffffu ? min_speed : 0;
}

/* ==========================================================================
 * Client enumeration
 * ========================================================================== */

void i2c_acpi_register_devices(struct i2c_adapter *adap)
{
	might_sleep();
	if (!adap->acpi_handle)
		return;
	mutex_lock(&g_acpi_scan_lock);
	if (i2c_acpi_scan_locked()) {
		mutex_unlock(&g_acpi_scan_lock);
		kprintf("i2c %s: failed to enumerate I2C slaves\n",
			adap->dev_name);
		return;
	}
	for (int i = 0; i < g_nnodes; i++) {
		struct i2c_acpi_node *n = &g_nodes[i];
		struct i2c_board_info info;
		struct acpi_device *adev;
		struct i2c_client *client;
		void *handle;

		/* The adapter must match the one in the I2cSerialBus. */
		if (n->adapter_handle != adap->acpi_handle || n->enumerated)
			continue;
		adev = acpi_dev_from_handle(n->handle);
		if (!adev)
			continue;
		if (!i2c_acpi_dev_present(adev) ||
		    !acpi_dev_ready_for_enumeration(adev)) {
			acpi_dev_put(adev);
			continue;
		}
		/* Boards known to describe bogus I2C devices. */
		if (i2c_acpi_quirk_skip_client_enumeration(adev)) {
			kprintf("i2c %s: ACPI device %s (%s) skipped (bogus tables on this board)\n",
				adap->dev_name, adev->path, n->name);
			acpi_dev_put(adev);
			continue;
		}
		kmemset(&info, 0, sizeof(info));
		i2c_strlcpy(info.type, n->name, sizeof(info.type));
		info.addr = n->addr;
		if (n->ten_bit)
			info.flags |= I2C_CLIENT_TEN;
		info.acpi_dev = adev;
		ksnprintf(info.dev_name, sizeof(info.dev_name), "i2c-%s",
			  n->name);
		kprintf("i2c %s: ACPI device %s (%s) at 0x%02x, %u kHz\n",
			adap->dev_name, adev->path, info.type, info.addr,
			n->speed / 1000);
		/* Marked before the client exists: its probe may run a scan
		 * of its own (nested adapters) that must not create it
		 * twice.  The scan lock is dropped around the creation, as
		 * probe() may call back into this file. */
		n->enumerated = 1;
		handle = n->handle;
		mutex_unlock(&g_acpi_scan_lock);
		client = i2c_new_client_device(adap, &info);
		acpi_dev_put(adev);
		mutex_lock(&g_acpi_scan_lock);
		/* The list does not change while the lock is dropped (it is
		 * only replaced by an invalidation, which tests do between
		 * scenarios); look the node up again anyway. */
		if (I2C_IS_ERR(client)) {
			for (int j = 0; j < g_nnodes; j++)
				if (g_nodes[j].handle == handle)
					g_nodes[j].enumerated = 0;
		} else {
			client->acpi_enumerated = 1;
		}
		if (i >= g_nnodes)
			break;
	}
	mutex_unlock(&g_acpi_scan_lock);
	/* The controller is ready: devices that depend on it (_DEP) may
	 * enumerate now. */
	acpi_dev_supplier_ready(adap->acpi_handle);
}

void i2c_acpi_forget_enumerated(struct i2c_client *client)
{
	if (!client->acpi_dev)
		return;
	mutex_lock(&g_acpi_scan_lock);
	for (int i = 0; i < g_nnodes; i++)
		if (g_nodes[i].handle == client->acpi_dev->handle)
			g_nodes[i].enumerated = 0;
	mutex_unlock(&g_acpi_scan_lock);
	client->acpi_enumerated = 0;
}

/* ==========================================================================
 * Client interrupt and power
 * ========================================================================== */

int i2c_acpi_get_irq(struct i2c_client *client, int *wake)
{
	struct acpi_res_list res;
	const struct acpi_res *r;
	int irq = -ENOENT, wake_capable = 0, rc;

	might_sleep();
	if (!client->acpi_dev)
		return -ENODEV;
	kmemset(&res, 0, sizeof(res));
	rc = acpi_dev_get_resources(client->acpi_dev, &res);
	if (rc && rc != -ENOENT)
		return rc;
	/* The first interrupt resource that is an I/O APIC input. */
	for (int i = 0; !rc && (r = acpi_res_find(&res, ACPI_RES_IRQ, i));
	     i++) {
		if (r->irq.source[0] || r->irq.producer)
			continue; /* another interrupt controller's input */
		irq = irq_alloc_desc_gsi(NULL, client, (int)r->irq.gsi,
					 r->irq.polarity == ACPI_RES_ACTIVE_LOW,
					 r->irq.triggering == ACPI_RES_LEVEL);
		if (irq > 0) {
			client->gsi_irq = irq;
			wake_capable = r->irq.wake_capable;
		} else {
			kprintf("i2c %s: GSI %u unavailable (%d)\n",
				client->dev_name, r->irq.gsi, irq);
		}
		break;
	}
	if (!rc)
		acpi_dev_free_resource_list(&res);

	if (irq == -ENOENT)
		irq = acpi_dev_gpio_irq_wake_get(client->acpi_dev, 0,
						 &wake_capable);
	if (irq < 0)
		return irq;
	if (wake)
		*wake = wake_capable;
	return irq;
}

void i2c_acpi_put_irq(struct i2c_client *client)
{
	if (client->gsi_irq > 0) {
		irq_free_desc(client->gsi_irq);
		client->gsi_irq = 0;
	}
}

void i2c_acpi_client_power(struct i2c_client *client, int on)
{
	struct acpi_device *adev = client->acpi_dev;
	int rc;

	if (!adev)
		return;
	if (!acpi_device_power_manageable(adev)) {
		client->powered = 0;
		return;
	}
	if (on) {
		rc = acpi_device_set_power(adev, ACPI_DEV_D0);
		if (rc)
			kprintf("i2c %s: cannot power up (%d)\n",
				client->dev_name, rc);
		client->powered = rc == 0;
		return;
	}
	/* The deepest state the device has in S0. */
	rc = acpi_device_set_power(adev, ACPI_DEV_D3_COLD);
	if (rc == -EINVAL)
		rc = acpi_device_set_power(adev, ACPI_DEV_D3_HOT);
	if (rc)
		kprintf("i2c %s: cannot power down (%d)\n", client->dev_name,
			rc);
	client->powered = 0;
}

int i2c_acpi_waive_d0_probe(struct i2c_client *client,
			    const struct i2c_driver *drv)
{
	return client->acpi_dev && (drv->flags & I2C_DRV_ACPI_WAIVE_D0_PROBE) &&
	       acpi_dev_has_property(client->acpi_dev->handle,
				     "i2c-allow-low-power-probe");
}

/* ==========================================================================
 * Bus recovery GPIOs
 * ========================================================================== */

/* The device-properties UUID of _DSD. */
static const char g_dsd_props_uuid[] = "daffd814-6eba-4d8c-8a91-bc9bbf4aa301";

/* The GPIO property `name' of the _DSD in `dsd': the device that owns
 * the GPIO resource, its index among that device's GPIO resources, and
 * the active-low flag.  0 or -ENOENT. */
static int dsd_find_gpio(const struct acpi_value *dsd, const char *name,
			 void **owner, int *crs_index, int *active_low)
{
	uint8_t uuid[16];

	if (!dsd || dsd->type != ACPI_VALUE_PACKAGE ||
	    acpi_uuid_parse(g_dsd_props_uuid, uuid))
		return -ENOENT;
	/* { UUID, { {name, value}, ... }, UUID, {...}, ... } */
	for (uint32_t i = 0; i + 1 < dsd->length; i += 2) {
		const struct acpi_value *u = &dsd->elements[i];
		const struct acpi_value *props = &dsd->elements[i + 1];

		if (u->type != ACPI_VALUE_BUFFER || u->length != 16 ||
		    kmemcmp(u->buffer, uuid, 16) ||
		    props->type != ACPI_VALUE_PACKAGE)
			continue;
		for (uint32_t j = 0; j < props->length; j++) {
			const struct acpi_value *p = &props->elements[j];
			const struct acpi_value *v;

			if (p->type != ACPI_VALUE_PACKAGE || p->length != 2 ||
			    p->elements[0].type != ACPI_VALUE_STRING ||
			    kstrcmp(p->elements[0].string, name))
				continue;
			v = &p->elements[1];
			/* { ^DEV, crs index, pin index, active low } */
			if (v->type != ACPI_VALUE_PACKAGE || v->length < 2 ||
			    v->elements[0].type != ACPI_VALUE_REFERENCE ||
			    v->elements[1].type != ACPI_VALUE_INTEGER)
				return -ENOENT;
			*owner = v->elements[0].handle;
			*crs_index = (int)v->elements[1].integer;
			*active_low = v->length >= 4 &&
				      v->elements[3].type ==
					      ACPI_VALUE_INTEGER &&
				      v->elements[3].integer != 0;
			return 0;
		}
	}
	return -ENOENT;
}

static int get_recovery_gpio(struct i2c_adapter *adap,
			     const struct acpi_value *dsd, const char *prop,
			     const char *label, enum gpiod_flags flags,
			     struct gpio_desc **out)
{
	struct acpi_device *owner;
	void *owner_handle = NULL;
	int crs_index = 0, active_low = 0, rc;

	*out = NULL;
	if (dsd_find_gpio(dsd, prop, &owner_handle, &crs_index, &active_low))
		return -ENOENT;
	if (!owner_handle || owner_handle == adap->acpi_handle)
		owner = acpi_dev_get(adap->acpi_dev);
	else
		owner = acpi_dev_from_handle(owner_handle);
	if (!owner)
		return -ENODEV;
	rc = acpi_dev_gpiod_get_index(owner, crs_index, label, flags, out);
	acpi_dev_put(owner);
	if (rc)
		return rc;
	if (active_low && !gpiod_is_active_low(*out))
		gpiod_toggle_active_low(*out);
	return 0;
}

int i2c_acpi_get_recovery_gpios(struct i2c_adapter *adap,
				struct gpio_desc **scl, struct gpio_desc **sda)
{
	struct acpi_value *dsd = NULL;
	int rc;

	*scl = NULL;
	*sda = NULL;
	if (!adap->acpi_dev || !adap->acpi_handle)
		return -ENOENT;
	if (!acpi_has_method(adap->acpi_handle, "_DSD"))
		return -ENOENT;
	rc = acpi_evaluate(adap->acpi_handle, "_DSD", NULL, 0, &dsd);
	if (rc)
		return rc == -ENOENT ? -ENOENT : rc;

	rc = get_recovery_gpio(adap, dsd, "scl-gpios", "i2c-scl",
			       GPIOD_OUT_HIGH, scl);
	if (rc) {
		acpi_value_free(dsd);
		return rc;
	}
	/* SCL is driven open drain: released high, pulled low. */
	(void)gpiod_set_config(*scl, PIN_CONFIG_DRIVE_OPEN_DRAIN, 0);

	/* SDA is optional; pull SCL low around taking it so that a glitch
	 * of SDA's configuration is not seen as a START or STOP. */
	(void)gpiod_direction_output(*scl, 0);
	udelay(10);
	rc = get_recovery_gpio(adap, dsd, "sda-gpios", "i2c-sda", GPIOD_IN,
			       sda);
	udelay(10);
	(void)gpiod_direction_output(*scl, 1);
	acpi_value_free(dsd);
	if (rc == -EPROBE_DEFER) {
		gpiod_free(*scl);
		*scl = NULL;
		return rc;
	}
	if (rc)
		*sda = NULL;
	return 0;
}

/* ==========================================================================
 * GenericSerialBus operation regions
 * ========================================================================== */

/* The handler's context: ACPICA stores the field's connection (the raw
 * I2cSerialBus descriptor) and access length in the leading
 * ACPI_CONNECTION_INFO before every call. */
struct i2c_acpi_gsb {
	ACPI_CONNECTION_INFO info;
	struct i2c_adapter *adap;
};

/* The data buffer of a GenericSerialBus access. */
struct gsb_buffer {
	uint8_t status;
	uint8_t len;
	union {
		uint16_t wdata;
		uint8_t bdata;
		uint8_t data[1];
	};
} __attribute__((packed));

/* Access attributes (the protocol of the field). */
#define GSB_ATTRIB_SEND_RCV 0x04
#define GSB_ATTRIB_BYTE 0x06
#define GSB_ATTRIB_WORD 0x08
#define GSB_ATTRIB_BLOCK 0x0a
#define GSB_ATTRIB_MULTIBYTE 0x0b

static int gsb_read_bytes(struct i2c_client *client, uint8_t cmd,
			  uint8_t *data, uint8_t data_len)
{
	struct i2c_msg msgs[2];
	uint8_t *buffer;
	int ret;

	buffer = kcalloc(1, (size_t)data_len + 1);
	if (!buffer)
		return -ENOMEM;
	msgs[0].addr = client->addr;
	msgs[0].flags = client->flags;
	msgs[0].len = 1;
	msgs[0].buf = &cmd;
	msgs[1].addr = client->addr;
	msgs[1].flags = (uint16_t)(client->flags | I2C_M_RD | I2C_M_DMA_SAFE);
	msgs[1].len = data_len;
	msgs[1].buf = buffer;
	ret = i2c_transfer(client->adapter, msgs, 2);
	if (ret < 0) {
		/* A NACK is normal with some tables. */
		if (ret != -EREMOTEIO && ret != -ENXIO)
			kprintf("i2c %s: i2c read %d bytes from client@%#x starting at reg %#x failed, error: %d\n",
				client->adapter->dev_name, data_len,
				client->addr, cmd, ret);
	} else if (ret == 2) {
		kmemcpy(data, buffer, data_len);
		ret = 0;
	} else {
		ret = -EIO;
	}
	kfree(buffer);
	return ret;
}

static int gsb_write_bytes(struct i2c_client *client, uint8_t cmd,
			   const uint8_t *data, uint8_t data_len)
{
	struct i2c_msg msg;
	uint8_t *buffer;
	int ret;

	buffer = kcalloc(1, (size_t)data_len + 1);
	if (!buffer)
		return -ENOMEM;
	buffer[0] = cmd;
	kmemcpy(buffer + 1, data, data_len);
	msg.addr = client->addr;
	msg.flags = (uint16_t)(client->flags | I2C_M_DMA_SAFE);
	msg.len = (uint16_t)(data_len + 1);
	msg.buf = buffer;
	ret = i2c_transfer(client->adapter, &msg, 1);
	kfree(buffer);
	if (ret < 0) {
		kprintf("i2c %s: i2c write failed: %d\n",
			client->adapter->dev_name, ret);
		return ret;
	}
	return ret == 1 ? 0 : -EIO;
}

static ACPI_STATUS i2c_acpi_space_handler(UINT32 function,
					  ACPI_PHYSICAL_ADDRESS command,
					  UINT32 bits, UINT64 *value64,
					  void *handler_context,
					  void *region_context)
{
	struct i2c_acpi_gsb *data = handler_context;
	struct gsb_buffer *gsb = (struct gsb_buffer *)value64;
	uint32_t accessor_type = function >> 16;
	uint8_t action = (uint8_t)(function & ACPI_IO_MASK);
	struct i2c_client client;
	struct acpi_res res;
	int status;

	(void)bits;
	(void)region_context;
	if (!data || !value64 || !data->info.Connection)
		return AE_BAD_PARAMETER;
	if (acpi_res_decode_buffer(data->info.Connection, data->info.Length,
				   &res) ||
	    res.type != ACPI_RES_I2C)
		return AE_BAD_PARAMETER;

	/* A throw-away client carrying the address; never registered. */
	kmemset(&client, 0, sizeof(client));
	client.adapter = data->adap;
	client.addr = res.i2c.address;
	if (res.i2c.ten_bit)
		client.flags |= I2C_CLIENT_TEN;

	switch (accessor_type) {
	case GSB_ATTRIB_SEND_RCV:
		if (action == ACPI_READ) {
			status = i2c_smbus_read_byte(&client);
			if (status >= 0) {
				gsb->bdata = (uint8_t)status;
				status = 0;
			}
		} else {
			status = i2c_smbus_write_byte(&client, gsb->bdata);
		}
		break;
	case GSB_ATTRIB_BYTE:
		if (action == ACPI_READ) {
			status = i2c_smbus_read_byte_data(&client,
							  (uint8_t)command);
			if (status >= 0) {
				gsb->bdata = (uint8_t)status;
				status = 0;
			}
		} else {
			status = i2c_smbus_write_byte_data(&client,
							   (uint8_t)command,
							   gsb->bdata);
		}
		break;
	case GSB_ATTRIB_WORD:
		if (action == ACPI_READ) {
			status = i2c_smbus_read_word_data(&client,
							  (uint8_t)command);
			if (status >= 0) {
				gsb->wdata = (uint16_t)status;
				status = 0;
			}
		} else {
			status = i2c_smbus_write_word_data(&client,
							   (uint8_t)command,
							   gsb->wdata);
		}
		break;
	case GSB_ATTRIB_BLOCK:
		if (action == ACPI_READ) {
			status = i2c_smbus_read_block_data(&client,
							   (uint8_t)command,
							   gsb->data);
			if (status >= 0) {
				gsb->len = (uint8_t)status;
				status = 0;
			}
		} else {
			status = i2c_smbus_write_block_data(&client,
							    (uint8_t)command,
							    gsb->len,
							    gsb->data);
		}
		break;
	case GSB_ATTRIB_MULTIBYTE:
		if (action == ACPI_READ)
			status = gsb_read_bytes(&client, (uint8_t)command,
						gsb->data,
						data->info.AccessLength);
		else
			status = gsb_write_bytes(&client, (uint8_t)command,
						 gsb->data,
						 data->info.AccessLength);
		break;
	default:
		kprintf("i2c %s: protocol 0x%02x not supported for client 0x%02x\n",
			data->adap->dev_name, accessor_type, client.addr);
		return AE_BAD_PARAMETER;
	}
	gsb->status = (uint8_t)status;
	return AE_OK;
}

int i2c_acpi_install_space_handler(struct i2c_adapter *adap)
{
	struct i2c_acpi_gsb *data;
	ACPI_STATUS st;

	if (!adap->acpi_handle)
		return -ENODEV;
	if (!acpi_dev_ready())
		return -ENODEV;
	data = kcalloc(1, sizeof(*data));
	if (!data)
		return -ENOMEM;
	data->adap = adap;
	st = AcpiInstallAddressSpaceHandler((ACPI_HANDLE)adap->acpi_handle,
					    ACPI_ADR_SPACE_GSBUS,
					    i2c_acpi_space_handler, NULL, data);
	if (ACPI_FAILURE(st)) {
		kfree(data);
		return -ENOMEM;
	}
	adap->acpi_gsb = data;
	return 0;
}

void i2c_acpi_remove_space_handler(struct i2c_adapter *adap)
{
	if (!adap->acpi_gsb)
		return;
	(void)AcpiRemoveAddressSpaceHandler((ACPI_HANDLE)adap->acpi_handle,
					    ACPI_ADR_SPACE_GSBUS,
					    i2c_acpi_space_handler);
	kfree(adap->acpi_gsb);
	adap->acpi_gsb = NULL;
}
