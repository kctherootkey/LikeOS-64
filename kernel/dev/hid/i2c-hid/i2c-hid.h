// LikeOS I2C-HID - HID over I2C transport
//
// A HID over I2C device (a touchpad, a touchscreen, a keyboard, a sensor
// hub) is a plain I2C target with a few 16-bit registers:
//
//   the HID descriptor register  (where firmware says: the _DSM of the
//       ACPI device, function 1 of the HID-over-I2C UUID) -- 30 bytes that
//       name every other register and the sizes involved;
//   the report descriptor register -- the HID report descriptor;
//   the input register -- read after the device asserted its interrupt:
//       a 2-byte little-endian length (the length itself included), then
//       the input report; a length of 0 is the device saying "reset done";
//   the command register -- RESET, SET_POWER, GET_REPORT, SET_REPORT, ...
//       as [register lo, hi, report type << 4 | report id, opcode
//       (, report id when it is 15 or more)];
//   the data register -- the payload of GET_REPORT / SET_REPORT, framed
//       with the same 2-byte length;
//   the output register -- output reports sent without a command.
//
// The transport reads the descriptors, brings the device out of reset and
// into the ON power state, and registers a HID device with the HID core
// (kernel/dev/hid/core/hid.h), implementing its struct hid_ll_driver:
// report requests become command/data register transfers, and the
// device's interrupt (a level-triggered line, usually a GPIO pad) becomes
// hid_input_report() calls from a threaded handler.
//
//   i2c-hid-acpi.c        the ACPI binding: PNP0C50 / ACPI0C50 devices,
//                         the HID descriptor register from _DSM
//   i2c-hid-core.c        the protocol, the hid_ll_driver, the interrupt,
//                         power management and error recovery
//   i2c-hid-dmi-quirks.c  machines whose touchpad firmware gives no (or
//                         wrong) descriptors: replacements by DMI match
//
// The transport only uses the I2C core's client interface (i2c.h):
// i2c_transfer() and friends.  It never touches controller registers.
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) 2012 Benjamin Tissoires, 2012 Ecole Nationale de l'Aviation Civile, 2012 Red Hat, Inc

#ifndef KERNEL_DEV_HID_I2C_HID_I2C_HID_H
#define KERNEL_DEV_HID_I2C_HID_I2C_HID_H

#include <kernel/uapi/types.h>
#include <kernel/dev/i2c/i2c.h>

struct hid_device;

/* The HID descriptor as the device sends it (little-endian fields). */
struct i2c_hid_desc {
	uint16_t wHIDDescLength; // 30
	uint16_t bcdVersion; // 0x0100
	uint16_t wReportDescLength;
	uint16_t wReportDescRegister;
	uint16_t wInputRegister;
	uint16_t wMaxInputLength; // length prefix included
	uint16_t wOutputRegister;
	uint16_t wMaxOutputLength;
	uint16_t wCommandRegister;
	uint16_t wDataRegister;
	uint16_t wVendorID;
	uint16_t wProductID;
	uint16_t wVersionID;
	uint32_t reserved;
} __attribute__((packed));

#define I2C_HID_DESC_LENGTH 30

/* Device quirks (ihid->quirks), from the vendor/product table in
 * i2c-hid-core.c. */
#define I2C_HID_QUIRK_NO_IRQ_AFTER_RESET (1u << 0) // reset is never acked
#define I2C_HID_QUIRK_BOGUS_IRQ (1u << 1) // interrupts with length 0xffff
#define I2C_HID_QUIRK_RESET_ON_RESUME (1u << 2) // reset, not just power on
#define I2C_HID_QUIRK_BAD_INPUT_SIZE (1u << 3) // the length prefix lies
#define I2C_HID_QUIRK_NO_WAKEUP_AFTER_RESET (1u << 4) // no SET_POWER ON then
#define I2C_HID_QUIRK_NO_SLEEP_ON_SUSPEND (1u << 5)
#define I2C_HID_QUIRK_DELAY_WAKEUP_AFTER_RESUME (1u << 6) // 1.5 s first
#define I2C_HID_QUIRK_RE_POWER_ON (1u << 7) // SET_POWER ON after setup

/* Power sequencing hooks of a binding (ACPI): all optional.  power_up /
 * power_down around probe, suspend (when the device may not wake the
 * system) and remove; shutdown_tail at the very end of a shutdown. */
struct i2chid_ops {
	int (*power_up)(struct i2chid_ops *ops);
	void (*power_down)(struct i2chid_ops *ops);
	void (*shutdown_tail)(struct i2chid_ops *ops);
};

/* The binding's probe hands over: the client (its interrupt resolved by
 * the I2C core), its power hooks, the HID descriptor register, and
 * HID_QUIRK_* bits for the HID device.  0 or -errno; on success the
 * client's driver data is the transport's state. */
int i2c_hid_core_probe(struct i2c_client *client, struct i2chid_ops *ops,
		       uint16_t hid_descriptor_address, uint32_t quirks);
void i2c_hid_core_remove(struct i2c_client *client);
/* The ops a bound client was probed with (NULL when not bound). */
struct i2chid_ops *i2c_hid_core_ops(struct i2c_client *client);
void i2c_hid_core_shutdown(struct i2c_client *client);
int i2c_hid_core_pm_suspend(struct i2c_client *client);
int i2c_hid_core_pm_resume(struct i2c_client *client);

/* The quirk bits for a vendor/product (exposed for the tests). */
uint32_t i2c_hid_lookup_quirk(uint16_t vendor, uint16_t product);

/* What the transport has seen, for diagnostics and the tests. */
struct i2c_hid_stats {
	unsigned long reports; // input reports handed to the HID core
	unsigned long resets_acked; // reset acknowledgements
	unsigned long spurious; // zero-length input with no reset pending
	unsigned long bogus; // 0xffff lengths of BOGUS_IRQ devices
	unsigned long bad_length; // lengths beyond the buffer or below 2
	unsigned long read_errors; // input reads that failed
	unsigned long recoveries; // device resets after I/O errors
	unsigned long reinits; // device-initiated resets answered
};
/* A snapshot of the client's counters; -ENODEV when it is not bound to
 * the transport. */
int i2c_hid_get_stats(struct i2c_client *client, struct i2c_hid_stats *out);
/* The HID device of a bound client, or NULL. */
struct hid_device *i2c_hid_get_hid(struct i2c_client *client);
/* The transport's line for /proc/i2c (the I2C driver's show hook):
 * counters of the recovery ladder (read errors, device resets, the
 * device's own resets, what is left of the recovery budget). */
long i2c_hid_core_show(struct i2c_client *client, char *buf, long cap,
		       long pos);

// ---------------------------------------------------------------------------
// DMI quirks (i2c-hid-dmi-quirks.c)
// ---------------------------------------------------------------------------

/* A replacement HID descriptor for client `i2c_name' on this machine, or
 * NULL. */
const struct i2c_hid_desc *i2c_hid_get_dmi_i2c_hid_desc_override(
	const char *i2c_name);
/* A replacement report descriptor (and its size), or NULL. */
const uint8_t *i2c_hid_get_dmi_hid_report_desc_override(const char *i2c_name,
							unsigned int *size);
/* HID_QUIRK_* bits this machine needs for the device. */
uint32_t i2c_hid_get_dmi_quirks(uint16_t vendor, uint16_t product);

// ---------------------------------------------------------------------------
// The ACPI binding (i2c-hid-acpi.c)
// ---------------------------------------------------------------------------

/* Register the I2C-HID ACPI driver with the I2C core (devices bind as
 * their adapters register, or right away when they already did).  Also
 * makes sure the HID core's drivers are registered.  Once, process
 * context; 0 or -errno. */
int i2c_hid_acpi_init(void);
void i2c_hid_acpi_exit(void);

#endif // KERNEL_DEV_HID_I2C_HID_I2C_HID_H
