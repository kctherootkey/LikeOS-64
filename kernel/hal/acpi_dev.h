// LikeOS ACPI - device layer over the ACPI namespace
//
// Drivers of devices that firmware describes in the ACPI namespace (I2C
// controllers and their clients, GPIO controllers, touchpads) need the
// same few things: find the device by its hardware id, read its current
// resources, evaluate a method or a _DSM, read a _DSD property, bring it
// to D0 and back.  This layer wraps ACPICA for exactly that, with types
// that do not drag ACPICA's headers into the drivers: handles are opaque
// pointers, resources come back as a decoded list, method results as a
// tree of struct acpi_value.
//
// Objects:
//   struct acpi_device   one namespace device: handle, full path, _HID,
//                        _CID list, _UID, _ADR, last _STA.  Reference
//                        counted; acpi_dev_put() frees it.  Namespace
//                        handles stay valid (tables are never unloaded),
//                        so two acpi_device objects of one handle are
//                        interchangeable snapshots.
//   struct acpi_res_list the decoded _CRS of a device (acpi_dev_get_
//                        resources), every descriptor in order.
//   struct acpi_value    an evaluated object (integer, string, buffer,
//                        package, reference); one allocation per tree,
//                        acpi_value_free() releases it.
//
// Every function fails cleanly (-ENODEV, NULL, 0) when ACPI is not
// initialised or the namespace has no such object.  Functions that
// evaluate AML may sleep (AML can wait, take mutexes and access slow
// operation regions): process context only.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_HAL_ACPI_DEV_H
#define KERNEL_HAL_ACPI_DEV_H

#include <kernel/uapi/types.h>
#include <kernel/uapi/bug.h>
#include <kernel/ke/sched.h>

struct pci_device;

#define ACPI_ID_LEN 16 /* _HID/_CID/_UID strings, NUL included */
#define ACPI_DEV_MAX_CIDS 8
#define ACPI_PATH_LEN 128 /* full namespace path, NUL included */

/* An id table entry; tables end with an entry whose id is "". */
struct acpi_device_id {
	char id[ACPI_ID_LEN];
	unsigned long driver_data;
};

/* _STA bits */
#define ACPI_DEV_STA_PRESENT 0x01u
#define ACPI_DEV_STA_ENABLED 0x02u
#define ACPI_DEV_STA_SHOW_IN_UI 0x04u
#define ACPI_DEV_STA_FUNCTIONING 0x08u
#define ACPI_DEV_STA_BATTERY 0x10u
#define ACPI_DEV_STA_DEFAULT 0x0fu /* no _STA: present and working */

struct acpi_device {
	void *handle; /* ACPI_HANDLE */
	char path[ACPI_PATH_LEN];
	char hid[ACPI_ID_LEN];
	char uid[ACPI_ID_LEN];
	char cid[ACPI_DEV_MAX_CIDS][ACPI_ID_LEN];
	int ncids;
	uint64_t adr;
	uint32_t sta;
	unsigned int has_hid : 1;
	unsigned int has_uid : 1;
	unsigned int has_adr : 1;
	refcount_t ref;
};

/* ACPICA is up and the namespace loaded. */
int acpi_dev_ready(void);
/* Does the platform sleep through low-power S0 idle (FADT flag) rather
 * than S3. */
int acpi_fadt_low_power_s0(void);

/* A snapshot of the device at `handle' (_STA evaluated).  NULL when the
 * object is not a device or memory ran out.  May sleep. */
struct acpi_device *acpi_dev_from_handle(void *handle);
/* The device at an absolute path ("\\_SB.PC00.I2C0"), or relative to
 * `scope' (a handle, NULL for the root).  May sleep. */
struct acpi_device *acpi_dev_from_path(void *scope, const char *path);
struct acpi_device *acpi_dev_get(struct acpi_device *adev);
void acpi_dev_put(struct acpi_device *adev);
/* Resolve a path (absolute, or relative to `scope') to a handle. */
void *acpi_get_handle_by_path(void *scope, const char *path);
/* The handle's parent device (NULL at the root). */
void *acpi_handle_parent(void *handle);
/* The full path of a handle into buf.  0 or -errno. */
int acpi_handle_path(void *handle, char *buf, size_t len);

/* Does the device carry `id' as its _HID or one of its _CIDs. */
int acpi_dev_has_id(const struct acpi_device *adev, const char *id);
/* The first entry of `ids' the device matches (_HID first, then the
 * _CIDs in order, as each id is tried against the whole table), or NULL. */
const struct acpi_device_id *
acpi_match_device_ids(const struct acpi_device *adev,
		      const struct acpi_device_id *ids);

/* _STA again (it changes: firmware hides and shows devices). */
uint32_t acpi_dev_update_status(struct acpi_device *adev);
static inline int acpi_dev_is_present(const struct acpi_device *adev)
{
	return (adev->sta & ACPI_DEV_STA_PRESENT) != 0;
}
/* Present and functioning (what enumeration requires; a device that is
 * present but not functioning is skipped as well). */
static inline int acpi_dev_is_usable(const struct acpi_device *adev)
{
	return (adev->sta & ACPI_DEV_STA_PRESENT) &&
	       (adev->sta & ACPI_DEV_STA_FUNCTIONING);
}

/* Enumerate every namespace device matching `ids' (_HID or _CID); with
 * `present_only' devices whose _STA says not present (or present but not
 * functioning) are skipped.  The callback gets a reference it may keep
 * (acpi_dev_get) -- the walk drops its own afterwards -- and returns
 * nonzero to stop.  Returns the number of devices passed to the callback
 * or -errno.  May sleep. */
typedef int (*acpi_dev_match_cb)(struct acpi_device *adev,
				 const struct acpi_device_id *id, void *ctx);
int acpi_dev_for_each_match(const struct acpi_device_id *ids,
			    int present_only, acpi_dev_match_cb cb,
			    void *ctx);
/* The same, collecting references instead of calling back: up to `max'
 * matching devices into out[] (each to be dropped with acpi_dev_put),
 * the count or -errno.  For probes that do real work per device: they
 * run after the namespace walk, not inside it. */
int acpi_dev_collect_matches(const struct acpi_device_id *ids,
			     int present_only, struct acpi_device **out,
			     int max);
/* Is a device with this _HID (and _UID, unless NULL) present.  `hrv' is
 * the hardware revision (_HRV) to require, -1 for any. */
int acpi_dev_present(const char *hid, const char *uid, int64_t hrv);
/* The direct children of `adev' that are devices, in namespace order;
 * the callback's reference handling and return as above. */
typedef int (*acpi_dev_child_cb)(struct acpi_device *child, void *ctx);
int acpi_dev_for_each_child(struct acpi_device *adev, acpi_dev_child_cb cb,
			    void *ctx);

/* The ACPI companion of a PCI function: the device whose _ADR is
 * (device << 16 | function) below the host bridge of the function's root
 * bus (_BBN, _SEG) or below the companion of the bridge above it.  NULL
 * when firmware describes none.  May sleep. */
struct acpi_device *acpi_pci_find_companion(const struct pci_device *pdev);

/* ---- evaluated objects -------------------------------------------------- */

enum acpi_value_type {
	ACPI_VALUE_NONE = 0,
	ACPI_VALUE_INTEGER,
	ACPI_VALUE_STRING,
	ACPI_VALUE_BUFFER,
	ACPI_VALUE_PACKAGE,
	ACPI_VALUE_REFERENCE, /* a namespace object (handle) */
};

struct acpi_value {
	enum acpi_value_type type;
	uint64_t integer; /* INTEGER */
	uint32_t length; /* STRING: chars (no NUL); BUFFER: bytes;
			  * PACKAGE: elements */
	union {
		char *string; /* NUL-terminated */
		uint8_t *buffer;
		struct acpi_value *elements;
	};
	void *handle; /* REFERENCE */
};

/* Free a result tree (NULL is fine). */
void acpi_value_free(struct acpi_value *v);

/* Evaluate `method' (a name or path relative to `handle'; NULL evaluates
 * `handle' itself) with up to 7 arguments.  *out (when non-NULL) receives
 * the result tree, NULL for a method returning nothing.  0, -ENODEV,
 * -ENOENT (no such object), -EIO (AML failed), -ENOMEM. */
int acpi_evaluate(void *handle, const char *method,
		  const struct acpi_value *args, int nargs,
		  struct acpi_value **out);
/* The same, the result must be an integer (a buffer of up to 8 bytes is
 * taken as little-endian integer, as the interpreter's implicit
 * conversion does). */
int acpi_evaluate_integer(void *handle, const char *method,
			  const struct acpi_value *args, int nargs,
			  uint64_t *val);
/* Does `handle' have a child object named `name' (a method usually). */
int acpi_has_method(void *handle, const char *name);
/* A method returning a package of integers: the first `n' elements as
 * 16-bit values (the I2C timing methods SSCN/FMCN/FPCN/HSCN return
 * {HCNT, LCNT, SDA hold}).  Returns the number of elements read (0..n)
 * or -errno; elements that are not integers end the read. */
int acpi_evaluate_u16_package(void *handle, const char *method,
			      uint16_t *vals, int n);
/* Evaluate _PS0/_PS3/_INI-style methods that take and return nothing:
 * 0, -ENOENT when absent, -EIO. */
int acpi_evaluate_void(void *handle, const char *method);

/* ---- _DSM --------------------------------------------------------------- */

/* "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" -> the 16-byte buffer layout of
 * an ACPI UUID (first three fields little-endian).  0 or -EINVAL. */
int acpi_uuid_parse(const char *str, uint8_t out[16]);
/* Evaluate _DSM(uuid, rev, func, arg).  `arg' is the fourth argument
 * (normally a package; NULL passes an empty package).  *out gets the
 * result tree (caller frees).  0 or -errno. */
int acpi_evaluate_dsm(void *handle, const uint8_t uuid[16], uint64_t rev,
		      uint64_t func, const struct acpi_value *arg,
		      struct acpi_value **out);
/* The same, requiring a result of `type' (-EPROTO otherwise). */
int acpi_evaluate_dsm_typed(void *handle, const uint8_t uuid[16],
			    uint64_t rev, uint64_t func,
			    const struct acpi_value *arg,
			    enum acpi_value_type type,
			    struct acpi_value **out);
/* Does the _DSM implement every function in `funcs' (a bit mask; bit 0
 * is the query function itself)?  Function 0 returns the supported set
 * as a buffer.  1 yes, 0 no. */
int acpi_check_dsm(void *handle, const uint8_t uuid[16], uint64_t rev,
		   uint64_t funcs);

/* ---- _DSD device properties ------------------------------------------- */

/* Properties under the device-properties UUID
 * daffd814-6eba-4d8c-8a91-bc9bbf4aa301.  0, -ENOENT (no such property or
 * no _DSD), -EPROTO (wrong type), -EOVERFLOW (value does not fit). */
int acpi_dev_get_property_u64(void *handle, const char *name, uint64_t *val);
int acpi_dev_get_property_u32(void *handle, const char *name, uint32_t *val);
int acpi_dev_get_property_u16(void *handle, const char *name, uint16_t *val);
int acpi_dev_get_property_u8(void *handle, const char *name, uint8_t *val);
/* A string property copied into buf (truncated, always terminated). */
int acpi_dev_get_property_string(void *handle, const char *name, char *buf,
				 size_t len);
/* An integer-array property (a package of integers, or one integer):
 * the number of elements stored (<= n) or -errno. */
int acpi_dev_get_property_u32_array(void *handle, const char *name,
				    uint32_t *vals, int n);
/* Is the property there at all (any type). */
int acpi_dev_has_property(void *handle, const char *name);
/* The index of `str' in a string-array property (a package of strings,
 * or one string), -ENODATA when it is not in the list, -errno. */
int acpi_dev_property_match_string(void *handle, const char *name,
				   const char *str);

/* Data-only child nodes of a _DSD: the hierarchical data extension
 * (UUID dbb8e3e6-5886-4ba6-8795-1319f52a966b) lists { "name", target }
 * links whose target -- a namespace path relative to the device, a
 * reference, or a package written in place -- is itself a _DSD-shaped
 * package with properties.  The nodes are kept in the order the
 * firmware lists them.  acpi_dev_get_data_nodes fills `nodes' (zeroed
 * by the caller or not: it is initialised) and returns the count, 0 when
 * the device has no such nodes, or -errno; free with
 * acpi_dev_free_data_nodes either way.  May sleep. */
#define ACPI_DSD_MAX_NODES 32
#define ACPI_DSD_NAME_LEN 32
struct acpi_dsd_nodes {
	struct acpi_value *dsd; /* the device's _DSD */
	int count;
	struct {
		char name[ACPI_DSD_NAME_LEN];
		const struct acpi_value *data; /* a _DSD-shaped package */
		struct acpi_value *owned; /* an evaluated target, or NULL */
	} node[ACPI_DSD_MAX_NODES];
};
int acpi_dev_get_data_nodes(void *handle, struct acpi_dsd_nodes *nodes);
void acpi_dev_free_data_nodes(struct acpi_dsd_nodes *nodes);
/* Properties of such a node (`data'): as the device getters above. */
int acpi_dsd_data_get_u32(const struct acpi_value *data, const char *name,
			  uint32_t *val);
int acpi_dsd_data_get_string(const struct acpi_value *data, const char *name,
			     char *buf, size_t len);

/* ---- resources ---------------------------------------------------------- */

enum acpi_res_type {
	ACPI_RES_MEM = 1, /* Memory24/32, FixedMemory32, Address16/32/64 */
	ACPI_RES_IO, /* IO, FixedIO, IO address space descriptors */
	ACPI_RES_IRQ, /* IRQ / ExtendedIRQ: one entry per interrupt */
	ACPI_RES_DMA, /* DMA / FixedDMA */
	ACPI_RES_I2C, /* I2cSerialBus(V2) */
	ACPI_RES_SPI, /* SpiSerialBus(V2) */
	ACPI_RES_UART, /* UartSerialBus(V2) */
	ACPI_RES_GPIO, /* GpioInt / GpioIo */
	ACPI_RES_OTHER, /* anything else (kept for index counting) */
};

/* Trigger and polarity (ACPI encoding). */
#define ACPI_RES_LEVEL 0
#define ACPI_RES_EDGE 1
#define ACPI_RES_ACTIVE_HIGH 0
#define ACPI_RES_ACTIVE_LOW 1
#define ACPI_RES_ACTIVE_BOTH 2

/* GPIO connection type, pull, IO restriction (ACPI encoding). */
#define ACPI_RES_GPIO_INT 0
#define ACPI_RES_GPIO_IO 1
#define ACPI_RES_PULL_DEFAULT 0
#define ACPI_RES_PULL_UP 1
#define ACPI_RES_PULL_DOWN 2
#define ACPI_RES_PULL_NONE 3
#define ACPI_RES_IO_RESTRICT_NONE 0
#define ACPI_RES_IO_RESTRICT_INPUT 1
#define ACPI_RES_IO_RESTRICT_OUTPUT 2
#define ACPI_RES_IO_RESTRICT_PRESERVE 3

#define ACPI_RES_GPIO_MAX_PINS 32

struct acpi_res_mem {
	uint64_t start;
	uint64_t length;
	uint8_t writeable;
	uint8_t producer; /* Address descriptors: produced for children */
	uint8_t cacheable; /* Address: memory type (0 non-cacheable) */
};

struct acpi_res_io {
	uint64_t start;
	uint64_t length;
};

struct acpi_res_irq {
	uint32_t gsi;
	uint8_t triggering; /* ACPI_RES_LEVEL / EDGE */
	uint8_t polarity; /* ACPI_RES_ACTIVE_* */
	uint8_t shareable;
	uint8_t wake_capable;
	uint8_t extended; /* from an ExtendedIRQ descriptor */
	uint8_t producer; /* ExtendedIRQ ResourceProducer */
	char source[ACPI_PATH_LEN]; /* ExtendedIRQ ResourceSource, "" */
};

struct acpi_res_dma {
	uint32_t channel;
	uint32_t request_line; /* FixedDMA */
	uint8_t width;
};

struct acpi_res_i2c {
	uint16_t address;
	uint8_t ten_bit; /* AddressingMode10Bit */
	uint8_t device_initiated; /* SlaveMode: DeviceInitiated */
	uint8_t producer;
	uint8_t shared; /* ConnectionSharing */
	uint8_t revision; /* 1: I2cSerialBus, 2: I2cSerialBusV2 */
	uint32_t speed_hz;
	char source[ACPI_PATH_LEN]; /* the controller */
	uint8_t source_index;
};

struct acpi_res_spi {
	uint32_t speed_hz;
	uint16_t chip_select;
	uint8_t mode; /* clock phase << 1 | polarity, as SPI mode */
	uint8_t word_bits;
	uint8_t wire3;
	uint8_t cs_active_high;
	uint8_t device_initiated;
	char source[ACPI_PATH_LEN];
};

struct acpi_res_uart {
	uint32_t baud;
	uint8_t data_bits, stop_bits, parity, flow_control;
	char source[ACPI_PATH_LEN];
};

struct acpi_res_gpio {
	uint8_t conn_type; /* ACPI_RES_GPIO_INT / _IO */
	uint8_t triggering; /* GpioInt only */
	uint8_t polarity; /* GpioInt: interrupt polarity; GpioIo: 0 */
	uint8_t shareable;
	uint8_t wake_capable;
	uint8_t pin_config; /* ACPI_RES_PULL_*, or vendor value >= 0x80 */
	uint8_t io_restriction; /* GpioIo: ACPI_RES_IO_RESTRICT_* */
	uint8_t producer;
	uint16_t drive_strength; /* hundredths of mA */
	uint16_t debounce_timeout; /* hundredths of ms (10 us units) */
	uint16_t npins; /* total in the descriptor */
	uint16_t pins[ACPI_RES_GPIO_MAX_PINS]; /* the first ones */
	char source[ACPI_PATH_LEN]; /* the GPIO controller */
	uint8_t source_index;
};

struct acpi_res {
	enum acpi_res_type type;
	union {
		struct acpi_res_mem mem;
		struct acpi_res_io io;
		struct acpi_res_irq irq;
		struct acpi_res_dma dma;
		struct acpi_res_i2c i2c;
		struct acpi_res_spi spi;
		struct acpi_res_uart uart;
		struct acpi_res_gpio gpio;
	};
};

struct acpi_res_list {
	int count;
	struct acpi_res *res;
};

/* Decode one ACPICA resource descriptor (an ACPI_RESOURCE *) into *out.
 * 0, or -ENOENT for an end tag / descriptor that carries nothing (the
 * dependent-function markers).  Interrupt descriptors listing several
 * interrupts decode to the first; acpi_get_resources() expands them. */
int acpi_res_decode_one(const void *ares, struct acpi_res *out);

/* Decode the first descriptor of a raw AML resource template (the
 * Connection() buffer of an operation region field).  0 or -errno. */
int acpi_res_decode_buffer(const uint8_t *aml, uint16_t len,
			   struct acpi_res *out);

/* Decode `method' (normally "_CRS") of `handle' into *list.  0, -ENODEV,
 * -ENOENT (no such method), -EIO, -ENOMEM.  Free with
 * acpi_dev_free_resource_list().  May sleep. */
int acpi_get_resources(void *handle, const char *method,
		       struct acpi_res_list *list);
static inline int acpi_dev_get_resources(struct acpi_device *adev,
					 struct acpi_res_list *list)
{
	return acpi_get_resources(adev->handle, "_CRS", list);
}
void acpi_dev_free_resource_list(struct acpi_res_list *list);
/* The `index'-th entry of `type' (0-based, in descriptor order), or NULL. */
const struct acpi_res *acpi_res_find(const struct acpi_res_list *list,
				     enum acpi_res_type type, int index);
int acpi_res_count(const struct acpi_res_list *list, enum acpi_res_type type);
/* The `index'-th GPIO descriptor of `conn_type' (ACPI_RES_GPIO_INT/_IO;
 * -1 for either), or NULL. */
const struct acpi_res_gpio *acpi_res_find_gpio(const struct acpi_res_list *list,
					       int conn_type, int index);

/* The `index'-th GpioInt of a device's _CRS: its controller (resolved
 * relative to the device), the first pin and the descriptor.  0, -ENOENT
 * (no such resource), -ENODEV (controller path unresolved), -errno. */
int acpi_dev_gpio_int(struct acpi_device *adev, int index, void **ctrl_handle,
		      unsigned int *pin, struct acpi_res_gpio *desc);

/* Decode an IRQ/GPIO trigger into IRQ_TYPE_* (kernel/ke/irq.h). */
unsigned int acpi_res_irq_type(uint8_t triggering, uint8_t polarity);

/* ---- device names ---------------------------------------------------------- */

/* The device's name, "<id>:<NN>": <id> its first id (_HID, else the first
 * _CID), NN its instance among the devices of that id, numbered the way
 * the ACPI scan creates its device objects (present or not, in namespace
 * order, devices with _DEP after the others; see acpi_dev.c), at least
 * two hex digits.  The name of the I2C client "i2c-<name>" and the key of
 * the GPIO ignore lists.  0, -ENOENT (no id), -ENODEV, -EINVAL.  May
 * sleep (the first call walks the namespace). */
int acpi_dev_name(void *handle, char *buf, size_t len);

/* The namespace was replaced: forget the device names and the power
 * bookkeeping (state and references) built from the old one. */
void acpi_dev_forget_namespace(void);

/* ---- power ---------------------------------------------------------------- */

enum acpi_power_state {
	ACPI_DEV_D0 = 0,
	ACPI_DEV_D1 = 1,
	ACPI_DEV_D2 = 2,
	ACPI_DEV_D3_HOT = 3,
	ACPI_DEV_D3_COLD = 4,
	ACPI_DEV_STATE_UNKNOWN = 0xff,
};

/* Move the device to `state': going up, the power resources of the
 * target state (_PRx) are turned on first and then _PSx is run; going
 * down, _PSx (_PS3 for both D3 states) runs first, then the resources the
 * new state no longer needs are released -- for D3cold all of them.
 * Power resources are reference counted across devices (_ON on the first
 * user unless already on, _OFF after the last); the counts start from
 * every present device in a powered state, and a resource the firmware
 * left on with no such device is never switched off.  Devices change
 * state independently (one lock each).  0, -ENODEV, -EIO, -EINVAL (a
 * state the device does not support).  May sleep. */
int acpi_device_set_power(struct acpi_device *adev, int state);
/* The state last set (or found: _PSC, or D0 when the device is powered
 * by resources that are on), ACPI_DEV_STATE_UNKNOWN when not known. */
int acpi_device_get_power(struct acpi_device *adev);
/* Does the device have power management (_PS0 or _PR0). */
int acpi_device_power_manageable(struct acpi_device *adev);
/* Is the device wake-capable (_PRW present). */
int acpi_device_can_wakeup(struct acpi_device *adev);

/* ---- driver-provided operation regions -------------------------------------- */

/* Serve AML accesses to a vendor address space `space_id' below `handle'
 * (and its children) from a register block: Address is the byte offset
 * into [base, base + length), accesses are 32-bit, serialised with
 * `lock' (may be NULL).  The block must stay mapped for good.  0 or
 * -errno. */
int acpi_install_mmio_region_handler(void *handle, uint8_t space_id,
				     uint8_t *base, uint64_t length,
				     spinlock_t *lock);

/* ---- _DEP ------------------------------------------------------------------ */

/* The suppliers listed in the device's _DEP (handles), at most `max'.
 * Returns the count or -errno. */
int acpi_dev_get_dependencies(struct acpi_device *adev, void **handles,
			      int max);
/* A supplier's driver says it is ready (a GPIO controller registered,
 * say): consumers waiting on it may enumerate. */
void acpi_dev_supplier_ready(void *handle);
int acpi_dev_supplier_is_ready(void *handle);
/* May the device be enumerated now.  Only suppliers of the kinds that
 * consumers really need first are waited for (camera PMICs, the vision
 * controller); dependencies on power-management or firmware-update
 * pseudo-devices, and everything else, never hold a device back, and a
 * supplier that is not present does not either.  1 ready, 0 not yet. */
int acpi_dev_ready_for_enumeration(struct acpi_device *adev);

#endif /* KERNEL_HAL_ACPI_DEV_H */
