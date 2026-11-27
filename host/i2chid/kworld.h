/* The boundary between the two halves of the I2C-HID harness
 * (host/test-i2chid.sh).
 *
 * The harness links two separately shimmed worlds into one program:
 *
 *   the bus world   the real I2C core, the real DesignWare driver with its
 *                   PCI glue, the real ACPI device layer and DMI matcher,
 *                   over the DesignWare register/bus model, the interrupt,
 *                   PCI and GPIO mocks and the ACPICA namespace model of
 *                   the I2C harness (host/i2c, host/kshim) -- plus the
 *                   I2C-HID device model on that bus (hidm.c) and the
 *                   machine fixtures (kworld.c);
 *   the HID world   the real I2C-HID transport, HID core, hid-multitouch,
 *                   input core, event devices and console pointer bridge,
 *                   over the input/HID harness's services (host/input,
 *                   host/hid) and the scenarios (tests.c).
 *
 * The two use different stand-ins for the kernel's locking headers, so
 * they cannot share objects: the bus world is linked into one relocatable
 * object whose symbols are all made local except the ones declared here.
 * The HID world reaches the bus world only through these functions -- the
 * transport's calls (i2c_transfer, request_threaded_irq, _DSM, ...) are
 * renamed to their kwb_ wrappers at compile time -- and every wrapper first
 * makes the calling thread known to the bus world.  Only plain types and
 * structures whose layout both worlds agree on (struct i2c_msg, the head
 * of struct i2c_client up to driver_data, struct acpi_value, struct
 * acpi_device_id, struct dmi_system_id) cross.
 *
 * Copyright (C) 2026 The LikeOS Project */
#ifndef HOST_I2CHID_KWORLD_H
#define HOST_I2CHID_KWORLD_H

#include <kernel/uapi/types.h>

struct i2c_adapter;
struct i2c_client;
struct i2c_driver;
struct i2c_msg;
struct acpi_device;
struct acpi_device_id;
struct acpi_value;
struct dmi_system_id;

/* ---- the transport's calls into the bus world --------------------------- */
int kwb_i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
int kwb_i2c_transfer_buffer_flags(const struct i2c_client *client, char *buf,
				  int count, uint16_t flags);
int kwb_i2c_smbus_read_byte(const struct i2c_client *client);
int kwb_i2c_register_driver(struct i2c_driver *drv);
void kwb_i2c_del_driver(struct i2c_driver *drv);
int kwb_i2c_client_devpath(const struct i2c_client *client, char *buf,
			   size_t len);
int kwb_request_threaded_irq(int irq, int (*fn)(void *),
			     int (*thread_fn)(void *), unsigned long flags,
			     const char *name, void *arg);
const char *kwb_free_irq(int irq, void *arg);
void kwb_disable_irq(int irq);
void kwb_disable_irq_nosync(int irq);
void kwb_enable_irq(int irq);
void kwb_synchronize_irq(int irq);
unsigned int kwb_irq_get_trigger_type(int irq);
int kwb_irq_set_irq_type(int irq, unsigned int type);
int kwb_irq_set_irq_wake(int irq, unsigned int on);
int kwb_acpi_uuid_parse(const char *str, uint8_t out[16]);
int kwb_acpi_evaluate_dsm_typed(void *handle, const uint8_t uuid[16],
				uint64_t rev, uint64_t func,
				const struct acpi_value *arg, int type,
				struct acpi_value **out);
void kwb_acpi_value_free(struct acpi_value *v);
const struct acpi_device_id *
kwb_acpi_match_device_ids(const struct acpi_device *adev,
			  const struct acpi_device_id *ids);
int kwb_acpi_evaluate_integer(void *handle, const char *method,
			      const struct acpi_value *args, int nargs,
			      uint64_t *val);
int kwb_acpi_evaluate(void *handle, const char *method,
		      const struct acpi_value *args, int nargs,
		      struct acpi_value **out);
int kwb_acpi_device_set_power(struct acpi_device *adev, int state);
struct acpi_device *kwb_acpi_dev_get(struct acpi_device *adev);
void kwb_acpi_dev_put(struct acpi_device *adev);
const struct dmi_system_id *kwb_dmi_first_match(const struct dmi_system_id *l);

/* ---- machines --------------------------------------------------------------- */

#define KW_HP 1 /* ProBook 650 G8: TGL-LP I2C0 00:15.0, ELAN073B at 0x15,
		 * GpioInt (level low, wake), descriptor register 0x0001 */
#define KW_DELL 2 /* Pro 16 Plus: ARL I2C0 + I2C3 00:15.3, VEN_0488 at
		   * 0x2c on I2C3, ExtendedIRQ GSI 27 (level low),
		   * descriptor register 0x0020 */
#define KW_SIPODEV 3 /* a Teclast F7: SYNA3602 at 0x2c whose firmware has no
		      * descriptors -- the DMI override table supplies them */

struct kw_cfg {
	int machine; /* KW_* */
	/* The device: report descriptor (copied), ids, wMaxInputLength
	 * (0: the longest input report + 2). */
	const uint8_t *rdesc;
	int rdesc_len;
	uint16_t vendor, product;
	uint16_t max_input;
	/* Descriptor faults. */
	uint16_t bcd; /* 0: 0x0100 */
	uint16_t desc_len; /* 0: 30 */
	/* _DSM: 0 returns the machine's register, 1 fails, 2 returns a
	 * value above 0xffff. */
	int dsm_mode;
	/* An ACPI _HID other than the machine's (the excluded CHPN0001). */
	const char *hid;
	/* GpioInt lookup answers -EPROBE_DEFER until kw_gpio_arrives(). */
	int gpio_defer;
	/* Not on the bus at all. */
	int absent;
};

/* Make the calling thread known to the bus world (the wrappers and the
 * kw_ calls do it themselves). */
void kw_adopt(void);
/* Build the machine: ACPI namespace, the controller(s) on the PCI model,
 * the device model on the bus, the I2C adapters with their clients.  The
 * I2C-HID driver is not registered yet. */
void kw_setup(const struct kw_cfg *cfg);
/* Unregister the controllers (clients unbound) and free the machine. */
void kw_teardown(void);
/* The touchpad's client (NULL when the core made none). */
struct i2c_client *kw_client(void);
/* The interrupt the client's line raises (> 0 once known). */
int kw_irq(void);
void kw_gpio_arrives(void);
int kw_suspend_all(void);
int kw_resume_all(void);
void kw_shutdown_all(void);
/* Warnings (WARN/WARN_ON) the bus world printed. */
long kw_warnings(void);
unsigned long kw_irq_threaded(int irq);
int kw_irq_depth(int irq);
int kw_irq_wake_enabled(int irq);
unsigned int kw_irq_trigger(int irq);
int kw_irqs_in_use(void);
/* The adapter's statistics: transfers, NACK errors, timeouts. */
void kw_adapter_stats(unsigned long *xfers, unsigned long *nacks,
		      unsigned long *timeouts);
/* The client's D-state as the ACPI layer last set it. */
int kw_client_power_state(void);
/* SMBIOS system information for the DMI matcher ("" clears). */
void kw_dmi_set(const char *vendor, const char *product);

/* The suspend test chain (i2c_pm_suspend_test): what the machine looked
 * like while asleep (taken when the GPIO controllers went down). */
struct kw_pm_snap {
	int taken;
	int client_suspended; /* the core's flag of the touchpad client */
	int adapter_suspended; /* the DesignWare system-suspend flag */
	int controller_off; /* runtime-suspended, LPSS function in reset */
	int irq_depth; /* the touchpad's interrupt disable depth */
	int irq_wake; /* its wake enable */
	int device_power; /* the device model: 0 ON, 1 SLEEP */
	unsigned long gpio_suspends, gpio_resumes;
};
int kw_suspend_test(unsigned int ms, struct kw_pm_snap *during);
unsigned long kw_gpio_resumes(void);
/* /proc/i2c as root reads it, and a write to it. */
long kw_proc_i2c(char *buf, long cap);
long kw_proc_i2c_write(const char *s);

/* ---- the I2C-HID device model (hidm.c) ------------------------------------- */

/* Faults (hidm_set). */
#define HIDM_NACK_WAKE 1 /* NACK the first address after SET_POWER SLEEP */
#define HIDM_NO_RESET_ACK 2 /* never acknowledge a reset */
#define HIDM_BOGUS 3 /* the next N input reads return length 0xffff */
#define HIDM_BAD_LEN 4 /* the next N reports claim a length > max */
#define HIDM_SHORT_LEN 5 /* the next N reports claim a length of 1 */
#define HIDM_GONE 6 /* NACK every address phase */
#define HIDM_NACK_READS 7 /* NACK the next N input read address phases */
#define HIDM_GARBAGE 8 /* the next N input reads return random bytes */
#define HIDM_SPONTANEOUS_RESET 9 /* queue N unsolicited 0-length reports */
#define HIDM_ASLEEP_REPORTS 10 /* report (assert the line) while asleep */
#define HIDM_LATE_RESET_ACK_MS 11 /* acknowledge resets this late */
#define HIDM_STRETCH_US 12 /* clock stretching per byte */

/* Counters and state (hidm_get). */
#define HIDM_RESETS 1
#define HIDM_POWER 2 /* 0 ON, 1 SLEEP */
#define HIDM_SET_POWER_ON 3
#define HIDM_SET_POWER_SLEEP 4
#define HIDM_SET_REPORTS 5
#define HIDM_GET_REPORTS 6
#define HIDM_INPUT_READS 7
#define HIDM_OUTPUT_REPORTS 8
#define HIDM_PROTO_ERRORS 9 /* malformed commands from the host */
#define HIDM_QUEUED 10 /* reports waiting */
#define HIDM_LINE 11 /* the interrupt line asserted */
#define HIDM_NACKS 12
#define HIDM_DESC_READS 13
#define HIDM_RDESC_READS 14
#define HIDM_LAST_SET_ID 15
#define HIDM_LONG_ID_CMDS 16 /* commands with the 0xF + id encoding */
#define HIDM_RESET_ACKS_SERVED 17

/* Store `len' bytes (ID first) as the answer to GET_REPORT `id'. */
void hidm_set_feature(int id, const uint8_t *data, int len);
/* The last SET_REPORT data for `id' (ID first): its length, 0 when none. */
int hidm_feature(int id, uint8_t *out, int max);
/* The last output report (ID first) and the register it went to. */
int hidm_output(uint8_t *out, int max, int *via_output_register);
/* Queue an input report (ID first; the model frames it). */
void hidm_push_report(const uint8_t *r, int len);
void hidm_set(int fault, int value);
int hidm_get(int what);
/* Wait until every queued report was read (or `ms' passed): 1 / 0. */
int hidm_wait_drained(int ms);
/* A deterministic pseudo-random sequence for faults. */
void hidm_seed(unsigned int seed);

#endif
