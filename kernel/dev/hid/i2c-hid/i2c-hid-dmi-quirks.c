// LikeOS I2C-HID - per-machine replacements for missing descriptors
//
// Some cheap touchpads (the SIPODEV SP1064 in a range of Windows laptops)
// supply no descriptors at all: on Windows a filter driver between the
// I2C-HID layer and the device injects them when the device is asked.
// The tables below are those descriptors, extracted by listening to the
// I2C-HID traffic between the filter driver and the I2C-HID driver; they
// are used on the machines the DMI table names, for the client the entry
// names.  A second table lists per-machine HID quirks for specific devices
// (a touchscreen mounted upside down).
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) 2018 Julian Sax

#include <kernel/dev/hid/i2c-hid/i2c-hid.h>
#include <kernel/dev/hid/core/hid.h>
#include <kernel/hal/dmi.h>
#include <kernel/io/console.h>

struct i2c_hid_desc_override {
	const uint8_t *i2c_hid_desc_buffer; // 30 bytes
	const uint8_t *hid_report_desc;
	unsigned int hid_report_desc_size;
	const char *i2c_name;
};

/* Descriptors of the SIPODEV SP1064 touchpad. */
static const uint8_t sipodev_hid_desc[I2C_HID_DESC_LENGTH] = {
	0x1e, 0x00,                  /* Length of descriptor                 */
	0x00, 0x01,                  /* Version of descriptor                */
	0xdb, 0x01,                  /* Length of report descriptor          */
	0x21, 0x00,                  /* Location of report descriptor        */
	0x24, 0x00,                  /* Location of input report             */
	0x1b, 0x00,                  /* Max input report length              */
	0x25, 0x00,                  /* Location of output report            */
	0x11, 0x00,                  /* Max output report length             */
	0x22, 0x00,                  /* Location of command register         */
	0x23, 0x00,                  /* Location of data register            */
	0x11, 0x09,                  /* Vendor ID                            */
	0x88, 0x52,                  /* Product ID                           */
	0x06, 0x00,                  /* Version ID                           */
	0x00, 0x00, 0x00, 0x00       /* Reserved                             */
};

static const uint8_t sipodev_report_desc[] = {
	0x05, 0x01,                  /* Usage Page (Desktop),                */
	0x09, 0x02,                  /* Usage (Mouse),                       */
	0xA1, 0x01,                  /* Collection (Application),            */
	0x85, 0x01,                  /*     Report ID (1),                   */
	0x09, 0x01,                  /*     Usage (Pointer),                 */
	0xA1, 0x00,                  /*     Collection (Physical),           */
	0x05, 0x09,                  /*         Usage Page (Button),         */
	0x19, 0x01,                  /*         Usage Minimum (01h),         */
	0x29, 0x02,                  /*         Usage Maximum (02h),         */
	0x25, 0x01,                  /*         Logical Maximum (1),         */
	0x75, 0x01,                  /*         Report Size (1),             */
	0x95, 0x02,                  /*         Report Count (2),            */
	0x81, 0x02,                  /*         Input (Variable),            */
	0x95, 0x06,                  /*         Report Count (6),            */
	0x81, 0x01,                  /*         Input (Constant),            */
	0x05, 0x01,                  /*         Usage Page (Desktop),        */
	0x09, 0x30,                  /*         Usage (X),                   */
	0x09, 0x31,                  /*         Usage (Y),                   */
	0x15, 0x81,                  /*         Logical Minimum (-127),      */
	0x25, 0x7F,                  /*         Logical Maximum (127),       */
	0x75, 0x08,                  /*         Report Size (8),             */
	0x95, 0x02,                  /*         Report Count (2),            */
	0x81, 0x06,                  /*         Input (Variable, Relative),  */
	0xC0,                        /*     End Collection,                  */
	0xC0,                        /* End Collection,                      */
	0x05, 0x0D,                  /* Usage Page (Digitizer),              */
	0x09, 0x05,                  /* Usage (Touchpad),                    */
	0xA1, 0x01,                  /* Collection (Application),            */
	0x85, 0x04,                  /*     Report ID (4),                   */
	0x05, 0x0D,                  /*     Usage Page (Digitizer),          */
	0x09, 0x22,                  /*     Usage (Finger),                  */
	0xA1, 0x02,                  /*     Collection (Logical),            */
	0x15, 0x00,                  /*         Logical Minimum (0),         */
	0x25, 0x01,                  /*         Logical Maximum (1),         */
	0x09, 0x47,                  /*         Usage (Touch Valid),         */
	0x09, 0x42,                  /*         Usage (Tip Switch),          */
	0x95, 0x02,                  /*         Report Count (2),            */
	0x75, 0x01,                  /*         Report Size (1),             */
	0x81, 0x02,                  /*         Input (Variable),            */
	0x95, 0x01,                  /*         Report Count (1),            */
	0x75, 0x03,                  /*         Report Size (3),             */
	0x25, 0x05,                  /*         Logical Maximum (5),         */
	0x09, 0x51,                  /*         Usage (Contact Identifier),  */
	0x81, 0x02,                  /*         Input (Variable),            */
	0x75, 0x01,                  /*         Report Size (1),             */
	0x95, 0x03,                  /*         Report Count (3),            */
	0x81, 0x03,                  /*         Input (Constant, Variable),  */
	0x05, 0x01,                  /*         Usage Page (Desktop),        */
	0x26, 0x44, 0x0A,            /*         Logical Maximum (2628),      */
	0x75, 0x10,                  /*         Report Size (16),            */
	0x55, 0x0E,                  /*         Unit Exponent (14),          */
	0x65, 0x11,                  /*         Unit (Centimeter),           */
	0x09, 0x30,                  /*         Usage (X),                   */
	0x46, 0x1A, 0x04,            /*         Physical Maximum (1050),     */
	0x95, 0x01,                  /*         Report Count (1),            */
	0x81, 0x02,                  /*         Input (Variable),            */
	0x46, 0xBC, 0x02,            /*         Physical Maximum (700),      */
	0x26, 0x34, 0x05,            /*         Logical Maximum (1332),      */
	0x09, 0x31,                  /*         Usage (Y),                   */
	0x81, 0x02,                  /*         Input (Variable),            */
	0xC0,                        /*     End Collection,                  */
	0x05, 0x0D,                  /*     Usage Page (Digitizer),          */
	0x09, 0x22,                  /*     Usage (Finger),                  */
	0xA1, 0x02,                  /*     Collection (Logical),            */
	0x25, 0x01,                  /*         Logical Maximum (1),         */
	0x09, 0x47,                  /*         Usage (Touch Valid),         */
	0x09, 0x42,                  /*         Usage (Tip Switch),          */
	0x95, 0x02,                  /*         Report Count (2),            */
	0x75, 0x01,                  /*         Report Size (1),             */
	0x81, 0x02,                  /*         Input (Variable),            */
	0x95, 0x01,                  /*         Report Count (1),            */
	0x75, 0x03,                  /*         Report Size (3),             */
	0x25, 0x05,                  /*         Logical Maximum (5),         */
	0x09, 0x51,                  /*         Usage (Contact Identifier),  */
	0x81, 0x02,                  /*         Input (Variable),            */
	0x75, 0x01,                  /*         Report Size (1),             */
	0x95, 0x03,                  /*         Report Count (3),            */
	0x81, 0x03,                  /*         Input (Constant, Variable),  */
	0x05, 0x01,                  /*         Usage Page (Desktop),        */
	0x26, 0x44, 0x0A,            /*         Logical Maximum (2628),      */
	0x75, 0x10,                  /*         Report Size (16),            */
	0x09, 0x30,                  /*         Usage (X),                   */
	0x46, 0x1A, 0x04,            /*         Physical Maximum (1050),     */
	0x95, 0x01,                  /*         Report Count (1),            */
	0x81, 0x02,                  /*         Input (Variable),            */
	0x46, 0xBC, 0x02,            /*         Physical Maximum (700),      */
	0x26, 0x34, 0x05,            /*         Logical Maximum (1332),      */
	0x09, 0x31,                  /*         Usage (Y),                   */
	0x81, 0x02,                  /*         Input (Variable),            */
	0xC0,                        /*     End Collection,                  */
	0x05, 0x0D,                  /*     Usage Page (Digitizer),          */
	0x09, 0x22,                  /*     Usage (Finger),                  */
	0xA1, 0x02,                  /*     Collection (Logical),            */
	0x25, 0x01,                  /*         Logical Maximum (1),         */
	0x09, 0x47,                  /*         Usage (Touch Valid),         */
	0x09, 0x42,                  /*         Usage (Tip Switch),          */
	0x95, 0x02,                  /*         Report Count (2),            */
	0x75, 0x01,                  /*         Report Size (1),             */
	0x81, 0x02,                  /*         Input (Variable),            */
	0x95, 0x01,                  /*         Report Count (1),            */
	0x75, 0x03,                  /*         Report Size (3),             */
	0x25, 0x05,                  /*         Logical Maximum (5),         */
	0x09, 0x51,                  /*         Usage (Contact Identifier),  */
	0x81, 0x02,                  /*         Input (Variable),            */
	0x75, 0x01,                  /*         Report Size (1),             */
	0x95, 0x03,                  /*         Report Count (3),            */
	0x81, 0x03,                  /*         Input (Constant, Variable),  */
	0x05, 0x01,                  /*         Usage Page (Desktop),        */
	0x26, 0x44, 0x0A,            /*         Logical Maximum (2628),      */
	0x75, 0x10,                  /*         Report Size (16),            */
	0x09, 0x30,                  /*         Usage (X),                   */
	0x46, 0x1A, 0x04,            /*         Physical Maximum (1050),     */
	0x95, 0x01,                  /*         Report Count (1),            */
	0x81, 0x02,                  /*         Input (Variable),            */
	0x46, 0xBC, 0x02,            /*         Physical Maximum (700),      */
	0x26, 0x34, 0x05,            /*         Logical Maximum (1332),      */
	0x09, 0x31,                  /*         Usage (Y),                   */
	0x81, 0x02,                  /*         Input (Variable),            */
	0xC0,                        /*     End Collection,                  */
	0x05, 0x0D,                  /*     Usage Page (Digitizer),          */
	0x09, 0x22,                  /*     Usage (Finger),                  */
	0xA1, 0x02,                  /*     Collection (Logical),            */
	0x25, 0x01,                  /*         Logical Maximum (1),         */
	0x09, 0x47,                  /*         Usage (Touch Valid),         */
	0x09, 0x42,                  /*         Usage (Tip Switch),          */
	0x95, 0x02,                  /*         Report Count (2),            */
	0x75, 0x01,                  /*         Report Size (1),             */
	0x81, 0x02,                  /*         Input (Variable),            */
	0x95, 0x01,                  /*         Report Count (1),            */
	0x75, 0x03,                  /*         Report Size (3),             */
	0x25, 0x05,                  /*         Logical Maximum (5),         */
	0x09, 0x51,                  /*         Usage (Contact Identifier),  */
	0x81, 0x02,                  /*         Input (Variable),            */
	0x75, 0x01,                  /*         Report Size (1),             */
	0x95, 0x03,                  /*         Report Count (3),            */
	0x81, 0x03,                  /*         Input (Constant, Variable),  */
	0x05, 0x01,                  /*         Usage Page (Desktop),        */
	0x26, 0x44, 0x0A,            /*         Logical Maximum (2628),      */
	0x75, 0x10,                  /*         Report Size (16),            */
	0x09, 0x30,                  /*         Usage (X),                   */
	0x46, 0x1A, 0x04,            /*         Physical Maximum (1050),     */
	0x95, 0x01,                  /*         Report Count (1),            */
	0x81, 0x02,                  /*         Input (Variable),            */
	0x46, 0xBC, 0x02,            /*         Physical Maximum (700),      */
	0x26, 0x34, 0x05,            /*         Logical Maximum (1332),      */
	0x09, 0x31,                  /*         Usage (Y),                   */
	0x81, 0x02,                  /*         Input (Variable),            */
	0xC0,                        /*     End Collection,                  */
	0x05, 0x0D,                  /*     Usage Page (Digitizer),          */
	0x55, 0x0C,                  /*     Unit Exponent (12),              */
	0x66, 0x01, 0x10,            /*     Unit (Seconds),                  */
	0x47, 0xFF, 0xFF, 0x00, 0x00,/*     Physical Maximum (65535),        */
	0x27, 0xFF, 0xFF, 0x00, 0x00,/*     Logical Maximum (65535),         */
	0x75, 0x10,                  /*     Report Size (16),                */
	0x95, 0x01,                  /*     Report Count (1),                */
	0x09, 0x56,                  /*     Usage (Scan Time),               */
	0x81, 0x02,                  /*     Input (Variable),                */
	0x09, 0x54,                  /*     Usage (Contact Count),           */
	0x25, 0x7F,                  /*     Logical Maximum (127),           */
	0x75, 0x08,                  /*     Report Size (8),                 */
	0x81, 0x02,                  /*     Input (Variable),                */
	0x05, 0x09,                  /*     Usage Page (Button),             */
	0x09, 0x01,                  /*     Usage (01h),                     */
	0x25, 0x01,                  /*     Logical Maximum (1),             */
	0x75, 0x01,                  /*     Report Size (1),                 */
	0x95, 0x01,                  /*     Report Count (1),                */
	0x81, 0x02,                  /*     Input (Variable),                */
	0x95, 0x07,                  /*     Report Count (7),                */
	0x81, 0x03,                  /*     Input (Constant, Variable),      */
	0x05, 0x0D,                  /*     Usage Page (Digitizer),          */
	0x85, 0x02,                  /*     Report ID (2),                   */
	0x09, 0x55,                  /*     Usage (Contact Count Maximum),   */
	0x09, 0x59,                  /*     Usage (59h),                     */
	0x75, 0x04,                  /*     Report Size (4),                 */
	0x95, 0x02,                  /*     Report Count (2),                */
	0x25, 0x0F,                  /*     Logical Maximum (15),            */
	0xB1, 0x02,                  /*     Feature (Variable),              */
	0x05, 0x0D,                  /*     Usage Page (Digitizer),          */
	0x85, 0x07,                  /*     Report ID (7),                   */
	0x09, 0x60,                  /*     Usage (60h),                     */
	0x75, 0x01,                  /*     Report Size (1),                 */
	0x95, 0x01,                  /*     Report Count (1),                */
	0x25, 0x01,                  /*     Logical Maximum (1),             */
	0xB1, 0x02,                  /*     Feature (Variable),              */
	0x95, 0x07,                  /*     Report Count (7),                */
	0xB1, 0x03,                  /*     Feature (Constant, Variable),    */
	0x85, 0x06,                  /*     Report ID (6),                   */
	0x06, 0x00, 0xFF,            /*     Usage Page (FF00h),              */
	0x09, 0xC5,                  /*     Usage (C5h),                     */
	0x26, 0xFF, 0x00,            /*     Logical Maximum (255),           */
	0x75, 0x08,                  /*     Report Size (8),                 */
	0x96, 0x00, 0x01,            /*     Report Count (256),              */
	0xB1, 0x02,                  /*     Feature (Variable),              */
	0xC0,                        /* End Collection,                      */
	0x06, 0x00, 0xFF,            /* Usage Page (FF00h),                  */
	0x09, 0x01,                  /* Usage (01h),                         */
	0xA1, 0x01,                  /* Collection (Application),            */
	0x85, 0x0D,                  /*     Report ID (13),                  */
	0x26, 0xFF, 0x00,            /*     Logical Maximum (255),           */
	0x19, 0x01,                  /*     Usage Minimum (01h),             */
	0x29, 0x02,                  /*     Usage Maximum (02h),             */
	0x75, 0x08,                  /*     Report Size (8),                 */
	0x95, 0x02,                  /*     Report Count (2),                */
	0xB1, 0x02,                  /*     Feature (Variable),              */
	0xC0,                        /* End Collection,                      */
	0x05, 0x0D,                  /* Usage Page (Digitizer),              */
	0x09, 0x0E,                  /* Usage (Configuration),               */
	0xA1, 0x01,                  /* Collection (Application),            */
	0x85, 0x03,                  /*     Report ID (3),                   */
	0x09, 0x22,                  /*     Usage (Finger),                  */
	0xA1, 0x02,                  /*     Collection (Logical),            */
	0x09, 0x52,                  /*         Usage (Device Mode),         */
	0x25, 0x0A,                  /*         Logical Maximum (10),        */
	0x95, 0x01,                  /*         Report Count (1),            */
	0xB1, 0x02,                  /*         Feature (Variable),          */
	0xC0,                        /*     End Collection,                  */
	0x09, 0x22,                  /*     Usage (Finger),                  */
	0xA1, 0x00,                  /*     Collection (Physical),           */
	0x85, 0x05,                  /*         Report ID (5),               */
	0x09, 0x57,                  /*         Usage (57h),                 */
	0x09, 0x58,                  /*         Usage (58h),                 */
	0x75, 0x01,                  /*         Report Size (1),             */
	0x95, 0x02,                  /*         Report Count (2),            */
	0x25, 0x01,                  /*         Logical Maximum (1),         */
	0xB1, 0x02,                  /*         Feature (Variable),          */
	0x95, 0x06,                  /*         Report Count (6),            */
	0xB1, 0x03,                  /*         Feature (Constant, Variable),*/
	0xC0,                        /*     End Collection,                  */
	0xC0                         /* End Collection                       */
};

static const struct i2c_hid_desc_override sipodev_desc = {
	.i2c_hid_desc_buffer = sipodev_hid_desc,
	.hid_report_desc = sipodev_report_desc,
	.hid_report_desc_size = sizeof(sipodev_report_desc),
	.i2c_name = "SYNA3602:00",
};

static const struct dmi_system_id i2c_hid_dmi_desc_override_table[] = {
	{
		.ident = "Teclast F6 Pro",
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "TECLAST"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "F6 Pro"),
		},
		.driver_data = &sipodev_desc
	},
	{
		.ident = "Teclast F7",
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "TECLAST"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "F7"),
		},
		.driver_data = &sipodev_desc
	},
	{
		.ident = "Trekstor Primebook C13",
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "TREKSTOR"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Primebook C13"),
		},
		.driver_data = &sipodev_desc
	},
	{
		.ident = "Trekstor Primebook C11",
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "TREKSTOR"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Primebook C11"),
		},
		.driver_data = &sipodev_desc
	},
	{
		/*
		 * There are at least 2 Primebook C11B versions, the older
		 * version has a product-name of "Primebook C11B", and a
		 * bios version / release / firmware revision of:
		 * V2.1.2 / 05/03/2018 / 18.2
		 * The new version has "PRIMEBOOK C11B" as product-name and a
		 * bios version / release / firmware revision of:
		 * CFALKSW05_BIOS_V1.1.2 / 11/19/2018 / 19.2
		 * Only the older version needs this quirk, note the newer
		 * version will not match as it has a different product-name.
		 */
		.ident = "Trekstor Primebook C11B",
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "TREKSTOR"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Primebook C11B"),
		},
		.driver_data = &sipodev_desc
	},
	{
		.ident = "Trekstor SURFBOOK E11B",
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "TREKSTOR"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "SURFBOOK E11B"),
		},
		.driver_data = &sipodev_desc
	},
	{
		.ident = "Direkt-Tek DTLAPY116-2",
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Direkt-Tek"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "DTLAPY116-2"),
		},
		.driver_data = &sipodev_desc
	},
	{
		.ident = "Direkt-Tek DTLAPY133-1",
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Direkt-Tek"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "DTLAPY133-1"),
		},
		.driver_data = &sipodev_desc
	},
	{
		.ident = "Mediacom Flexbook Edge 11",
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "MEDIACOM"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "FlexBook edge11 - M-FBE11"),
		},
		.driver_data = &sipodev_desc
	},
	{
		.ident = "Mediacom FlexBook edge 13",
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "MEDIACOM"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "FlexBook_edge13-M-FBE13"),
		},
		.driver_data = &sipodev_desc
	},
	{
		.ident = "Odys Winbook 13",
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "AXDIA International GmbH"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "WINBOOK 13"),
		},
		.driver_data = &sipodev_desc
	},
	{
		.ident = "iBall Aer3",
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "iBall"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Aer3"),
		},
		.driver_data = &sipodev_desc
	},
	{
		.ident = "Schneider SCL142ALM",
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "SCHNEIDER"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "SCL142ALM"),
		},
		.driver_data = &sipodev_desc
	},
	{
		.ident = "Vero K147",
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "VERO"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "K147"),
		},
		.driver_data = &sipodev_desc
	},
	{ 0 } /* Terminate list */
};

/* A touchscreen mounted upside down: both axes inverted on that machine
 * only. */
static const struct hid_device_id i2c_hid_elan_flipped_quirks = {
	HID_DEVICE(BUS_I2C, HID_GROUP_MULTITOUCH_WIN_8, USB_VENDOR_ID_ELAN,
		   0x2dcd),
	.driver_data = HID_QUIRK_X_INVERT | HID_QUIRK_Y_INVERT
};

/* Devices with issues that depend on the machine they are built into, not
 * only on the device itself: driver_data names the device to match. */
static const struct dmi_system_id i2c_hid_dmi_quirk_table[] = {
	{
		.ident = "DynaBook K50/FR",
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Dynabook Inc."),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "dynabook K50/FR"),
		},
		.driver_data = &i2c_hid_elan_flipped_quirks,
	},
	{ 0 } /* Terminate list */
};

static const struct i2c_hid_desc_override *
i2c_hid_dmi_override_for(const char *i2c_name)
{
	const struct dmi_system_id *system_id;
	const struct i2c_hid_desc_override *override;

	system_id = dmi_first_match(i2c_hid_dmi_desc_override_table);
	if (!system_id || !i2c_name)
		return NULL;
	override = system_id->driver_data;
	if (!override || kstrcmp(override->i2c_name, i2c_name))
		return NULL;
	return override;
}

const struct i2c_hid_desc *i2c_hid_get_dmi_i2c_hid_desc_override(
	const char *i2c_name)
{
	const struct i2c_hid_desc_override *override =
		i2c_hid_dmi_override_for(i2c_name);

	if (!override)
		return NULL;
	BUILD_BUG_ON(sizeof(struct i2c_hid_desc) != sizeof(sipodev_hid_desc));
	return (const struct i2c_hid_desc *)override->i2c_hid_desc_buffer;
}

const uint8_t *i2c_hid_get_dmi_hid_report_desc_override(const char *i2c_name,
							unsigned int *size)
{
	const struct i2c_hid_desc_override *override =
		i2c_hid_dmi_override_for(i2c_name);

	if (!override)
		return NULL;
	*size = override->hid_report_desc_size;
	return override->hid_report_desc;
}

uint32_t i2c_hid_get_dmi_quirks(uint16_t vendor, uint16_t product)
{
	const struct dmi_system_id *system_id =
		dmi_first_match(i2c_hid_dmi_quirk_table);
	uint32_t quirks = 0;

	if (system_id) {
		const struct hid_device_id *device_id = system_id->driver_data;

		if (device_id && device_id->vendor == vendor &&
		    device_id->product == product)
			quirks = (uint32_t)device_id->driver_data;
	}
	return quirks;
}
