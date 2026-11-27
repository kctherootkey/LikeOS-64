// LikeOS HAL - the platform identification in sysfs
//
// /sys/devices/virtual/dmi/id (and /sys/class/dmi/id): one read-only file
// per SMBIOS field the firmware provides (kernel/hal/dmi.c), serial
// numbers and the system UUID readable by root only, `modalias' with the
// "dmi:bvn...:" match string and `uevent' carrying it as MODALIAS=.  Hardware
// databases and input quirk tables in user space key on these.
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) 2007 Lennart Poettering

#include <kernel/hal/dmi.h>
#include <kernel/fs/sysfs.h>
#include <kernel/io/console.h>
#include <kernel/ke/sched.h>
#include <kernel/uapi/bug.h>

static long dmi_show_field(struct pfs_node *n, char *buf, long cap)
{
	return pfs_printf(buf, cap, 0, "%s\n",
			  dmi_get_system_info((enum dmi_field)n->arg2));
}

static long dmi_show_modalias(struct pfs_node *n, char *buf, long cap)
{
	long len;

	(void)n;
	len = dmi_get_modalias(buf, cap);
	if (len + 1 < cap)
		buf[len] = '\n';
	return len + 1;
}

static long dmi_show_uevent(struct pfs_node *n, char *buf, long cap)
{
	static const char key[] = "MODALIAS=";
	long k = (long)sizeof(key) - 1, len;

	(void)n;
	if (cap > k)
		kmemcpy(buf, key, (size_t)k);
	len = dmi_get_modalias(cap > k ? buf + k : buf, cap > k ? cap - k : 0);
	if (k + len + 1 < cap)
		buf[k + len] = '\n';
	return k + len + 1;
}

void dmi_sysfs_init(void)
{
	static const struct {
		const char *name;
		enum dmi_field field;
		uint32_t mode;
	} attrs[] = {
		{ "bios_vendor", DMI_BIOS_VENDOR, 0444 },
		{ "bios_version", DMI_BIOS_VERSION, 0444 },
		{ "bios_date", DMI_BIOS_DATE, 0444 },
		{ "sys_vendor", DMI_SYS_VENDOR, 0444 },
		{ "bios_release", DMI_BIOS_RELEASE, 0444 },
		{ "ec_firmware_release", DMI_EC_FIRMWARE_RELEASE, 0444 },
		{ "product_name", DMI_PRODUCT_NAME, 0444 },
		{ "product_version", DMI_PRODUCT_VERSION, 0444 },
		{ "product_serial", DMI_PRODUCT_SERIAL, 0400 },
		{ "product_uuid", DMI_PRODUCT_UUID, 0400 },
		{ "product_sku", DMI_PRODUCT_SKU, 0444 },
		{ "product_family", DMI_PRODUCT_FAMILY, 0444 },
		{ "board_vendor", DMI_BOARD_VENDOR, 0444 },
		{ "board_name", DMI_BOARD_NAME, 0444 },
		{ "board_version", DMI_BOARD_VERSION, 0444 },
		{ "board_serial", DMI_BOARD_SERIAL, 0400 },
		{ "board_asset_tag", DMI_BOARD_ASSET_TAG, 0444 },
		{ "chassis_vendor", DMI_CHASSIS_VENDOR, 0444 },
		{ "chassis_type", DMI_CHASSIS_TYPE, 0444 },
		{ "chassis_version", DMI_CHASSIS_VERSION, 0444 },
		{ "chassis_serial", DMI_CHASSIS_SERIAL, 0400 },
		{ "chassis_asset_tag", DMI_CHASSIS_ASSET_TAG, 0444 },
	};
	static int done;
	char base[96], path[160];
	struct pfs_node t;

	might_sleep();
	if (done)
		return;
	done = 1;
	if (!dmi_field_present(DMI_BIOS_VENDOR) &&
	    !dmi_field_present(DMI_SYS_VENDOR) &&
	    !dmi_field_present(DMI_BOARD_VENDOR))
		return; /* no SMBIOS: no dmi device */
	if (sysfs_add_class_device("dmi", "id", NULL, base, sizeof(base)))
		return;
	for (unsigned int i = 0; i < sizeof(attrs) / sizeof(attrs[0]); i++) {
		/* Only what the firmware provides. */
		if (!dmi_field_present(attrs[i].field))
			continue;
		kmemset(&t, 0, sizeof(t));
		t.show = dmi_show_field;
		t.arg2 = (uint64_t)attrs[i].field;
		t.mode = attrs[i].mode;
		ksnprintf(path, sizeof(path), "%s/%s", base, attrs[i].name);
		(void)sysfs_add_node(path, PFS_FILE, &t);
	}
	kmemset(&t, 0, sizeof(t));
	t.show = dmi_show_modalias;
	t.mode = 0444;
	ksnprintf(path, sizeof(path), "%s/modalias", base);
	(void)sysfs_add_node(path, PFS_FILE, &t);
	t.show = dmi_show_uevent;
	ksnprintf(path, sizeof(path), "%s/uevent", base);
	(void)sysfs_add_node(path, PFS_FILE, &t);
}

