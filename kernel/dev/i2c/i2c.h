// LikeOS I2C - the generic I2C/SMBus core
//
// Three kinds of object meet here:
//
//   struct i2c_adapter  one bus master: a controller driver (DesignWare,
//                       kernel/dev/i2c/busses) fills in an algorithm --
//                       how to run a list of messages on its bus -- and
//                       registers it as bus N ("i2c-N").  The core owns
//                       the bus lock, the retry loop, the quirk checks,
//                       the suspend gate, bus recovery and the clients.
//   struct i2c_client   one device on a bus: an address (7 or 10 bit),
//                       flags, an interrupt, and -- for devices firmware
//                       describes -- the ACPI device it came from.  Clients
//                       of ACPI-described adapters are created by the core
//                       when the adapter registers (i2c-core-acpi.c), named
//                       after their ACPI device ("ELAN073B:00", device
//                       "i2c-ELAN073B:00").
//   struct i2c_driver   a client driver (a touchpad, a codec, a sensor):
//                       an ACPI id table and/or an I2C id table, probe()
//                       and remove().  The core binds drivers to clients
//                       whenever either side appears.
//
// Layering: a client driver only ever uses this header -- i2c_transfer()
// and friends -- and never touches controller registers; a controller
// driver only implements struct i2c_algorithm and never knows what the
// devices on its bus are.
//
// Transfers.  i2c_transfer(adap, msgs, num) runs `num' messages as one
// combined transaction: START, the first message, a repeated START before
// each further message (whatever its direction), a single STOP after the
// last.  It returns `num' on success or a negative errno:
//   -EREMOTEIO  the address or a data byte was not acknowledged (nobody
//               at that address, the device is asleep, or it refused the
//               data); the DesignWare controller does not tell the two
//               apart in its errno, as callers must not rely on it
//   -ENXIO      (other adapters) the address was not acknowledged
//   -EAGAIN     arbitration lost (after the adapter's retries)
//   -ETIMEDOUT  the transfer did not finish within adapter->timeout; the
//               controller was reset and bus recovery attempted
//   -EOPNOTSUPP the adapter cannot do this transfer (zero-length message,
//               message too long, too many messages: adapter quirks)
//   -ESHUTDOWN  the adapter is suspended
//   -EINVAL     bad arguments (no messages, invalid address)
//   -EIO        any other bus or controller error
//   -EPROTO     (SMBus) the device returned an invalid block length
//   -EBADMSG    (SMBus) packet error check failed
// i2c_transfer() takes the bus lock and may sleep; with interrupts off
// (shutdown paths) it uses the adapter's atomic transfer method and only
// tries the lock (-EAGAIN when busy).
//
// Client probe.  When a driver is bound, the core first resolves the
// client's interrupt for an ACPI client -- the first Interrupt/
// ExtendedIRQ resource, otherwise the first GpioInt (whose controller may
// not be there yet: then probe() is not called and the client is retried
// later, "deferred probe") -- sets client->irq (0: none) and
// I2C_CLIENT_WAKE when firmware marked it a wake source, puts the ACPI
// device in D0, and calls probe(client).  A probe() returning
// -EPROBE_DEFER is retried after every adapter and driver registration
// and at the end of the boot-time I2C initialisation.  When probe() fails
// or remove() has run, the device goes back to low power and client->irq
// to what it was before.
//
// Locking: the bus lock (a sleeping mutex) serialises transfers per
// adapter; the core's registry lock (a mutex) protects the adapter table,
// the driver table and the client lists and is never held across a
// driver callback.  Driver callbacks of one client are serialised by the
// client's bind lock.
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) 1995-2000 Simon G. Vogl, 2013-2017 Wolfram Sang

#ifndef KERNEL_DEV_I2C_I2C_H
#define KERNEL_DEV_I2C_I2C_H

#include <kernel/uapi/types.h>
#include <kernel/uapi/bug.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/mutex.h>
#include <kernel/ke/waitq.h>
#include <kernel/ke/list.h>

/* Error numbers the I2C code uses that not every build environment
 * defines. */
#ifndef ESHUTDOWN
#define ESHUTDOWN 108 /* the adapter is suspended */
#endif
#ifndef EPROBE_DEFER
#define EPROBE_DEFER 517 /* a resource's provider is not there yet */
#endif
#ifndef EBADMSG
#define EBADMSG 74
#endif
#ifndef ENOTTY
#define ENOTTY 25
#endif
#ifndef EACCES
#define EACCES 13
#endif
#ifndef ENOTSUPP
#define ENOTSUPP 524
#endif
#ifndef ENAMETOOLONG
#define ENAMETOOLONG 36
#endif

struct acpi_device;
struct acpi_device_id;
struct gpio_desc;
struct pfs_node;

#define I2C_NAME_SIZE 20
#define I2C_DEV_NAME_SIZE 32
#define I2C_ADAPTER_NAME_SIZE 48
/* Bus numbers handed out (adapter->nr 0 .. I2C_MAX_ADAPTERS-1). */
#define I2C_MAX_ADAPTERS 64
#define I2C_MAX_DRIVERS 64

/* Standard bus speeds (Hz). */
#define I2C_MAX_STANDARD_MODE_FREQ 100000u
#define I2C_MAX_FAST_MODE_FREQ 400000u
#define I2C_MAX_FAST_MODE_PLUS_FREQ 1000000u
#define I2C_MAX_TURBO_MODE_FREQ 1400000u
#define I2C_MAX_HIGH_SPEED_MODE_FREQ 3400000u
#define I2C_MAX_ULTRA_FAST_MODE_FREQ 5000000u

/* ---- messages (the layout of the user interface's struct i2c_msg) ---- */

struct i2c_msg {
	uint16_t addr; /* 7- or 10-bit target address, no direction bit */
	uint16_t flags;
#define I2C_M_RD 0x0001 /* read (always 0x0001) */
#define I2C_M_TEN 0x0010 /* 10-bit address (== I2C_CLIENT_TEN) */
#define I2C_M_DMA_SAFE 0x0200 /* buffer is heap memory */
#define I2C_M_RECV_LEN 0x0400 /* first byte read is the length */
#define I2C_M_NO_RD_ACK 0x0800 /* protocol mangling: */
#define I2C_M_IGNORE_NAK 0x1000 /*   (I2C_FUNC_PROTOCOL_MANGLING) */
#define I2C_M_REV_DIR_ADDR 0x2000
#define I2C_M_NOSTART 0x4000 /* I2C_FUNC_NOSTART */
#define I2C_M_STOP 0x8000 /* STOP after this message */
	uint16_t len;
	uint8_t *buf;
};

/* Adapter functionality. */
#define I2C_FUNC_I2C 0x00000001u
#define I2C_FUNC_10BIT_ADDR 0x00000002u
#define I2C_FUNC_PROTOCOL_MANGLING 0x00000004u
#define I2C_FUNC_SMBUS_PEC 0x00000008u
#define I2C_FUNC_NOSTART 0x00000010u
#define I2C_FUNC_SLAVE 0x00000020u
#define I2C_FUNC_SMBUS_BLOCK_PROC_CALL 0x00008000u
#define I2C_FUNC_SMBUS_QUICK 0x00010000u
#define I2C_FUNC_SMBUS_READ_BYTE 0x00020000u
#define I2C_FUNC_SMBUS_WRITE_BYTE 0x00040000u
#define I2C_FUNC_SMBUS_READ_BYTE_DATA 0x00080000u
#define I2C_FUNC_SMBUS_WRITE_BYTE_DATA 0x00100000u
#define I2C_FUNC_SMBUS_READ_WORD_DATA 0x00200000u
#define I2C_FUNC_SMBUS_WRITE_WORD_DATA 0x00400000u
#define I2C_FUNC_SMBUS_PROC_CALL 0x00800000u
#define I2C_FUNC_SMBUS_READ_BLOCK_DATA 0x01000000u
#define I2C_FUNC_SMBUS_WRITE_BLOCK_DATA 0x02000000u
#define I2C_FUNC_SMBUS_READ_I2C_BLOCK 0x04000000u
#define I2C_FUNC_SMBUS_WRITE_I2C_BLOCK 0x08000000u
#define I2C_FUNC_SMBUS_HOST_NOTIFY 0x10000000u
#define I2C_FUNC_SMBUS_BYTE \
	(I2C_FUNC_SMBUS_READ_BYTE | I2C_FUNC_SMBUS_WRITE_BYTE)
#define I2C_FUNC_SMBUS_BYTE_DATA \
	(I2C_FUNC_SMBUS_READ_BYTE_DATA | I2C_FUNC_SMBUS_WRITE_BYTE_DATA)
#define I2C_FUNC_SMBUS_WORD_DATA \
	(I2C_FUNC_SMBUS_READ_WORD_DATA | I2C_FUNC_SMBUS_WRITE_WORD_DATA)
#define I2C_FUNC_SMBUS_BLOCK_DATA \
	(I2C_FUNC_SMBUS_READ_BLOCK_DATA | I2C_FUNC_SMBUS_WRITE_BLOCK_DATA)
#define I2C_FUNC_SMBUS_I2C_BLOCK \
	(I2C_FUNC_SMBUS_READ_I2C_BLOCK | I2C_FUNC_SMBUS_WRITE_I2C_BLOCK)
#define I2C_FUNC_SMBUS_EMUL                                               \
	(I2C_FUNC_SMBUS_QUICK | I2C_FUNC_SMBUS_BYTE |                     \
	 I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA |            \
	 I2C_FUNC_SMBUS_PROC_CALL | I2C_FUNC_SMBUS_WRITE_BLOCK_DATA |     \
	 I2C_FUNC_SMBUS_I2C_BLOCK | I2C_FUNC_SMBUS_PEC)
#define I2C_FUNC_SMBUS_EMUL_ALL                                           \
	(I2C_FUNC_SMBUS_EMUL | I2C_FUNC_SMBUS_READ_BLOCK_DATA |           \
	 I2C_FUNC_SMBUS_BLOCK_PROC_CALL)

/* ---- SMBus ------------------------------------------------------------- */

#define I2C_SMBUS_BLOCK_MAX 32
union i2c_smbus_data {
	uint8_t byte;
	uint16_t word;
	uint8_t block[I2C_SMBUS_BLOCK_MAX + 2]; /* [0]: length */
};
#define I2C_SMBUS_READ 1
#define I2C_SMBUS_WRITE 0
#define I2C_SMBUS_QUICK 0
#define I2C_SMBUS_BYTE 1
#define I2C_SMBUS_BYTE_DATA 2
#define I2C_SMBUS_WORD_DATA 3
#define I2C_SMBUS_PROC_CALL 4
#define I2C_SMBUS_BLOCK_DATA 5
#define I2C_SMBUS_I2C_BLOCK_BROKEN 6
#define I2C_SMBUS_BLOCK_PROC_CALL 7
#define I2C_SMBUS_I2C_BLOCK_DATA 8

/* ---- clients --------------------------------------------------------------- */

/* client->flags (also accepted in i2c_board_info.flags) */
#define I2C_CLIENT_PEC 0x04 /* SMBus packet error checking */
#define I2C_CLIENT_TEN 0x10 /* 10-bit address (== I2C_M_TEN) */
#define I2C_CLIENT_SLAVE 0x20 /* the adapter is the target */
#define I2C_CLIENT_HOST_NOTIFY 0x40 /* SMBus host notify interrupt */
#define I2C_CLIENT_WAKE 0x80 /* the interrupt is a wake source */
#define I2C_CLIENT_SCCB 0x9000 /* Omnivision SCCB protocol */

/* Address encoding used by the busy check. */
#define I2C_ADDR_OFFSET_TEN_BIT 0xa000
#define I2C_ADDR_OFFSET_SLAVE 0x1000

/* What a driver matches on besides ACPI ids: the client's name. */
struct i2c_device_id {
	char name[I2C_NAME_SIZE];
	unsigned long driver_data;
};

struct i2c_adapter;
struct i2c_driver;

#define I2C_CLIENT_MAGIC 0x49324343u /* "I2CC" */

struct i2c_client {
	unsigned short flags; /* I2C_CLIENT_* */
	unsigned short addr;
	char name[I2C_NAME_SIZE]; /* "ELAN073B:00", or the board type */
	char dev_name[I2C_DEV_NAME_SIZE]; /* "i2c-ELAN073B:00", "0-0050" */
	struct i2c_adapter *adapter;
	struct i2c_driver *driver; /* bound driver, NULL when none */
	int init_irq; /* what the creator gave (0: look it up) */
	int irq; /* the interrupt to request, 0: none */
	struct acpi_device *acpi_dev; /* firmware node, or NULL (a reference) */
	/* The table entry the driver matched with (one of them non-NULL
	 * while bound). */
	const struct acpi_device_id *acpi_id;
	const struct i2c_device_id *id;
	void *driver_data; /* i2c_set_clientdata */

	/* ---- the core's ---- */
	uint32_t magic;
	refcount_t ref;
	struct list_head node; /* adapter->clients */
	struct mutex bind_lock; /* probe/remove/suspend of this client */
	void *volatile probing_task; /* the task in its probe(), or NULL */
	unsigned short init_flags;
	uint8_t dead; /* unregistered: never bound again */
	uint8_t deferred; /* last probe asked to be retried */
	uint8_t acpi_enumerated; /* created by the ACPI scan */
	uint8_t suspended; /* driver suspend() has run */
	uint8_t powered; /* the core put the ACPI device in D0 */
	uint8_t wake_armed; /* enable_irq_wake done for suspend */
	int gsi_irq; /* interrupt descriptor the core made for a GSI */
	int probe_err; /* last probe result, for diagnostics */
	unsigned long probe_attempts;
};

/* What a client is created from. */
struct i2c_board_info {
	char type[I2C_NAME_SIZE]; /* becomes client->name */
	unsigned short flags; /* I2C_CLIENT_* */
	unsigned short addr;
	int irq; /* 0: none (ACPI clients: looked up at probe) */
	struct acpi_device *acpi_dev; /* firmware node (a new reference is
				       * taken), NULL when none */
	char dev_name[I2C_DEV_NAME_SIZE]; /* "" : "<bus>-<addr>" */
};

#define I2C_BOARD_INFO(dev_type, dev_addr) .type = dev_type, .addr = (dev_addr)

/* ---- drivers ------------------------------------------------------------------ */

/* i2c_driver.flags */
#define I2C_DRV_ACPI_WAIVE_D0_PROBE (1u << 0) /* probe in low power when
					       * firmware allows it */

enum i2c_alert_protocol {
	I2C_PROTOCOL_SMBUS_ALERT,
	I2C_PROTOCOL_SMBUS_HOST_NOTIFY,
};

struct i2c_driver {
	const char *name;
	unsigned int flags; /* I2C_DRV_* */
	/* Bind: 0 claims the client; -EPROBE_DEFER asks to be retried
	 * later; anything else leaves the client unbound (the next
	 * matching driver may try).  May sleep. */
	int (*probe)(struct i2c_client *client);
	/* Unbind (driver unregistration, client or adapter removal).  May
	 * sleep; transfers still work while it runs. */
	void (*remove)(struct i2c_client *client);
	/* System shutdown: quiesce the device.  May sleep. */
	void (*shutdown)(struct i2c_client *client);
	/* System suspend/resume of the client (before its adapter suspends,
	 * after it resumed).  May sleep. */
	int (*suspend)(struct i2c_client *client);
	int (*resume)(struct i2c_client *client);
	/* SMBus alert / host notify (not raised by any adapter here yet). */
	void (*alert)(struct i2c_client *client,
		      enum i2c_alert_protocol protocol, unsigned int data);
	/* Driver-specific in-kernel commands. */
	int (*command)(struct i2c_client *client, unsigned int cmd,
		       void *arg);
	/* Optional: append the client's driver state (counters) to
	 * /proc/i2c, as i2c_show_printf does.  Called with the client's
	 * binding held, so the driver data stays. */
	long (*show)(struct i2c_client *client, char *buf, long cap, long pos);
	const struct i2c_device_id *id_table; /* ends with name "" */
	const struct acpi_device_id *acpi_match_table; /* ends with id "" */

	/* ---- the core's ---- */
	int registered;
	int binding; /* probes in flight */
	int slot;
};

/* ---- adapters ---------------------------------------------------------------- */

struct i2c_algorithm {
	/* Run `num' messages as one combined transaction: `num' or -errno.
	 * Called with the bus lock held, process context. */
	int (*master_xfer)(struct i2c_adapter *adap, struct i2c_msg *msgs,
			   int num);
	/* The same with interrupts off: must not sleep (polls). */
	int (*master_xfer_atomic)(struct i2c_adapter *adap,
				  struct i2c_msg *msgs, int num);
	/* Native SMBus (NULL: emulated over master_xfer). -EOPNOTSUPP for a
	 * protocol the hardware cannot do falls back to emulation. */
	int (*smbus_xfer)(struct i2c_adapter *adap, uint16_t addr,
			  unsigned short flags, char read_write,
			  uint8_t command, int size,
			  union i2c_smbus_data *data);
	int (*smbus_xfer_atomic)(struct i2c_adapter *adap, uint16_t addr,
				 unsigned short flags, char read_write,
				 uint8_t command, int size,
				 union i2c_smbus_data *data);
	/* I2C_FUNC_* */
	uint32_t (*functionality)(struct i2c_adapter *adap);
};

/* What the controller cannot do; the core refuses such transfers with
 * -EOPNOTSUPP before they reach the driver.  A 0 limit means none. */
struct i2c_adapter_quirks {
	uint64_t flags;
	int max_num_msgs;
	uint16_t max_write_len;
	uint16_t max_read_len;
	uint16_t max_comb_1st_msg_len;
	uint16_t max_comb_2nd_msg_len;
};
/* Only "write then read" pairs as combined messages: */
#define I2C_AQ_COMB (1u << 0) /* two messages at most */
#define I2C_AQ_COMB_WRITE_FIRST (1u << 1) /* the first one a write */
#define I2C_AQ_COMB_READ_SECOND (1u << 2) /* the second one a read */
#define I2C_AQ_COMB_SAME_ADDR (1u << 3) /* both to the same address */
#define I2C_AQ_COMB_WRITE_THEN_READ                                       \
	(I2C_AQ_COMB | I2C_AQ_COMB_WRITE_FIRST | I2C_AQ_COMB_READ_SECOND | \
	 I2C_AQ_COMB_SAME_ADDR)
#define I2C_AQ_NO_CLK_STRETCH (1u << 4)
#define I2C_AQ_NO_ZERO_LEN_READ (1u << 5)
#define I2C_AQ_NO_ZERO_LEN_WRITE (1u << 6)
#define I2C_AQ_NO_ZERO_LEN (I2C_AQ_NO_ZERO_LEN_READ | I2C_AQ_NO_ZERO_LEN_WRITE)
#define I2C_AQ_NO_REP_START (1u << 7)

/* Bus recovery: clock SCL until a target that holds SDA low lets go,
 * then generate a STOP.  A controller driver provides either its own
 * recover_bus, or the line accessors for i2c_generic_scl_recovery (set
 * directly, or GPIO lines that the core then drives). */
struct i2c_bus_recovery_info {
	int (*recover_bus)(struct i2c_adapter *adap);
	int (*get_scl)(struct i2c_adapter *adap);
	void (*set_scl)(struct i2c_adapter *adap, int val);
	int (*get_sda)(struct i2c_adapter *adap);
	void (*set_sda)(struct i2c_adapter *adap, int val);
	int (*get_bus_free)(struct i2c_adapter *adap); /* 1 free, 0 busy */
	void (*prepare_recovery)(struct i2c_adapter *adap);
	void (*unprepare_recovery)(struct i2c_adapter *adap);
	struct gpio_desc *scl_gpiod;
	struct gpio_desc *sda_gpiod;
};

/* System suspend/resume of the adapter itself (after/before its
 * clients'); usually i2c_mark_adapter_suspended() + power down. */
struct i2c_adapter_pm_ops {
	int (*suspend)(struct i2c_adapter *adap);
	int (*resume)(struct i2c_adapter *adap);
	/* System shutdown, after the clients' shutdown. */
	void (*shutdown)(struct i2c_adapter *adap);
};

/* Transfer statistics (the core's; /proc/i2c). */
struct i2c_adapter_stats {
	uint64_t xfers; /* i2c_transfer calls that reached the driver */
	uint64_t msgs;
	uint64_t bytes;
	uint64_t ok;
	uint64_t err_nack; /* -ENXIO / -EREMOTEIO */
	uint64_t err_arb; /* -EAGAIN (final) */
	uint64_t err_timeout;
	uint64_t err_other;
	uint64_t retries; /* attempts repeated after -EAGAIN */
	uint64_t recoveries; /* i2c_recover_bus calls */
	uint64_t recovery_failed;
	uint64_t rejected_quirk;
	uint64_t rejected_suspended;
	uint64_t smbus; /* SMBus transactions */
};

/* adapter->locked_flags */
#define I2C_ALF_IS_SUSPENDED 0
#define I2C_ALF_SUSPEND_REPORTED 1

/* i2c_lock_bus() flags.  Without multiplexers both lock the adapter's
 * own bus lock. */
#define I2C_LOCK_ROOT_ADAPTER (1u << 0)
#define I2C_LOCK_SEGMENT (1u << 1)

/* Bus lock operations of an adapter whose bus needs more than the
 * adapter's own lock (a bus shared with firmware: the arbitration is
 * taken around the lock).  They call the default operations
 * (i2c_adapter_lock_bus ...) for the lock itself.  NULL: the defaults. */
struct i2c_lock_operations {
	void (*lock_bus)(struct i2c_adapter *adap, unsigned int flags);
	int (*trylock_bus)(struct i2c_adapter *adap, unsigned int flags);
	void (*unlock_bus)(struct i2c_adapter *adap, unsigned int flags);
};

#define I2C_CLASS_HWMON (1u << 0)
#define I2C_CLASS_SPD (1u << 7)
#define I2C_CLASS_DEPRECATED (1u << 8)

struct i2c_adapter {
	/* ---- filled by the controller driver ---- */
	char name[I2C_ADAPTER_NAME_SIZE];
	unsigned int class;
	const struct i2c_algorithm *algo;
	void *algo_data;
	const struct i2c_adapter_quirks *quirks;
	struct i2c_bus_recovery_info *bus_recovery_info;
	const struct i2c_adapter_pm_ops *pm;
	const struct i2c_lock_operations *lock_ops; /* NULL: the defaults */
	int timeout; /* per transfer, ms (0: 1000) */
	int retries; /* repeats after lost arbitration */
	int nr; /* bus number for i2c_add_numbered_adapter, -1: any */
	/* The ACPI companion (the controller's namespace device) whose
	 * children's I2cSerialBus resources name this adapter; the driver
	 * keeps its own reference. */
	struct acpi_device *acpi_dev;
	void *acpi_handle;
	/* "<driver> <device>" for log lines, e.g. "i2c_designware
	 * 0000:00:15.0". */
	char parent_name[48];
	uint32_t bus_freq_hz; /* informational */
	/* Optional: append controller details to /proc/i2c. */
	long (*show)(struct i2c_adapter *adap, char *buf, long cap,
		     long pos);

	/* ---- the core's ---- */
	char dev_name[16]; /* "i2c-N" */
	struct mutex bus_lock;
	void *volatile bus_lock_owner; /* the task holding bus_lock */
	volatile unsigned long locked_flags; /* I2C_ALF_* bits */
	refcount_t ref; /* 1 while registered + i2c_get_adapter users */
	struct wait_queue_head ref_wq;
	struct list_head clients; /* struct i2c_client.node */
	int registered;
	int dying; /* i2c_del_adapter in progress */
	int pm_suspended; /* i2c_pm_suspend_all ran pm->suspend */
	void *adapdata; /* i2c_set_adapdata */
	struct i2c_adapter_stats stats;
	void *i2c_dev; /* the /dev/i2c-N node (i2c-dev.c) */
	void *acpi_gsb; /* GenericSerialBus region handler (core-acpi) */
};

static inline void *i2c_get_adapdata(const struct i2c_adapter *adap)
{
	return adap->adapdata;
}
static inline void i2c_set_adapdata(struct i2c_adapter *adap, void *data)
{
	adap->adapdata = data;
}
static inline void *i2c_get_clientdata(const struct i2c_client *client)
{
	return client->driver_data;
}
static inline void i2c_set_clientdata(struct i2c_client *client, void *data)
{
	client->driver_data = data;
}
static inline int i2c_adapter_id(const struct i2c_adapter *adap)
{
	return adap->nr;
}
/* The address byte on the wire: address << 1 | read. */
static inline uint8_t i2c_8bit_addr_from_msg(const struct i2c_msg *msg)
{
	return (uint8_t)((msg->addr << 1) | (msg->flags & I2C_M_RD));
}

/* ---- adapter registration (process context, may sleep) ------------------ */

/* Register with a dynamic bus number (the lowest free one).  Then:
 * bus recovery is set up, /dev/i2c-N appears, the ACPI clients of the
 * adapter are created and drivers bound.  0 or -errno. */
int i2c_add_adapter(struct i2c_adapter *adap);
/* With adap->nr as bus number (-EBUSY when taken); nr -1: dynamic. */
int i2c_add_numbered_adapter(struct i2c_adapter *adap);
/* Unregister: clients are unbound and removed, the node goes, and the
 * call waits until no i2c_get_adapter() reference is left.  Afterwards
 * the driver may free the adapter. */
void i2c_del_adapter(struct i2c_adapter *adap);
/* A reference to bus `nr' (NULL when none or going away). */
struct i2c_adapter *i2c_get_adapter(int nr);
void i2c_put_adapter(struct i2c_adapter *adap);
/* The adapter whose ACPI companion is `handle' (a reference), or NULL. */
struct i2c_adapter *i2c_find_adapter_by_acpi_handle(void *handle);

/* ---- drivers ----------------------------------------------------------------- */

/* Register a driver (it must stay valid until i2c_del_driver) and bind
 * it to every matching unbound client.  0 or -errno.  May sleep. */
int i2c_register_driver(struct i2c_driver *drv);
#define i2c_add_driver(d) i2c_register_driver(d)
/* Unbind from every client and forget the driver.  May sleep. */
void i2c_del_driver(struct i2c_driver *drv);

/* ---- clients ------------------------------------------------------------------ */

/* Create a client on `adap' and bind a driver if one matches.  Returns
 * the client (a reference owned by the adapter's list: do not put it;
 * i2c_unregister_device() drops it), or an error pointer: test with
 * I2C_IS_ERR(), -EINVAL bad address, -EBUSY address in use, -ENOMEM.
 * May sleep. */
struct i2c_client *i2c_new_client_device(struct i2c_adapter *adap,
					 const struct i2c_board_info *info);
/* A client with no driver, reserving `address' (secondary addresses of
 * a multi-address device). */
struct i2c_client *i2c_new_dummy_device(struct i2c_adapter *adap,
					uint16_t address);
/* Unbind and remove (NULL and error pointers are ignored). */
void i2c_unregister_device(struct i2c_client *client);
/* Extra references for code that keeps a client pointer. */
struct i2c_client *i2c_client_get(struct i2c_client *client);
void i2c_client_put(struct i2c_client *client);
/* `ptr' if it is a live client, else NULL. */
struct i2c_client *i2c_verify_client(void *ptr);
/* The driver data of the table entry the bound driver matched with
 * (ACPI table first, then the I2C id table); 0 when none. */
unsigned long i2c_get_match_data(const struct i2c_client *client);
/* The client of `adap' at `addr' (a reference), or NULL. */
struct i2c_client *i2c_find_client(struct i2c_adapter *adap, uint16_t addr);
/* The client's place in the device tree, for the devices layered on it
 * (HID devices, input devices): built from the adapter's parent_name
 * ("<driver> <device>").  A PCI controller "i2c_designware 0000:00:15.0"
 * on bus 0 gives "/devices/pci0000:00/0000:00:15.0/i2c_designware.0/
 * i2c-0/i2c-ELAN073B:00", an ACPI-described one "/devices/platform/
 * <device>/i2c-N/<client>", an adapter without a parent "/devices/i2c-N/
 * <client>".  0, -EINVAL, -ENAMETOOLONG (`buf' too short). */
int i2c_client_devpath(const struct i2c_client *client, char *buf, size_t len);

#define I2C_ERR_PTR(e) ((void *)(long)(e))
#define I2C_PTR_ERR(p) ((long)(p))
#define I2C_IS_ERR(p) ((unsigned long)(p) >= (unsigned long)-4095)
#define I2C_IS_ERR_OR_NULL(p) (!(p) || I2C_IS_ERR(p))

/* Address checks: 0 or -EINVAL.  7-bit: 0x01..0x7f (0 is the general
 * call); 10-bit: up to 0x3ff.  The strict form rejects the reserved
 * 7-bit addresses 0x00-0x07 and 0x78-0x7f (for probing). */
int i2c_check_addr_validity(unsigned int addr, unsigned short flags);
int i2c_check_7bit_addr_validity_strict(unsigned short addr);

/* Bind (or retry binding) every unbound client.  Called by the core
 * itself after each adapter and driver registration and each successful
 * probe; providers of resources clients defer on (GPIO controllers) may
 * call it too.  May sleep. */
void i2c_retry_deferred_probes(void);

/* ---- transfers ---------------------------------------------------------------- */

/* See the top of this file.  May sleep (atomic only with interrupts
 * off and an atomic-capable adapter). */
int i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
/* The same with the bus lock already held by the caller. */
int __i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
/* One message to/from client->addr: `count' or -errno. */
int i2c_transfer_buffer_flags(const struct i2c_client *client, char *buf,
			      int count, uint16_t flags);
static inline int i2c_master_send(const struct i2c_client *client,
				  const char *buf, int count)
{
	return i2c_transfer_buffer_flags(client, (char *)buf, count, 0);
}
static inline int i2c_master_recv(const struct i2c_client *client, char *buf,
				  int count)
{
	return i2c_transfer_buffer_flags(client, buf, count, I2C_M_RD);
}
static inline int i2c_master_send_dmasafe(const struct i2c_client *client,
					  const char *buf, int count)
{
	return i2c_transfer_buffer_flags(client, (char *)buf, count,
					 I2C_M_DMA_SAFE);
}
static inline int i2c_master_recv_dmasafe(const struct i2c_client *client,
					  char *buf, int count)
{
	return i2c_transfer_buffer_flags(client, buf, count,
					 I2C_M_RD | I2C_M_DMA_SAFE);
}

uint32_t i2c_get_functionality(struct i2c_adapter *adap);
static inline int i2c_check_functionality(struct i2c_adapter *adap,
					  uint32_t func)
{
	return (func & i2c_get_functionality(adap)) == func;
}
/* Does a transfer in this context use the atomic path (interrupts off,
 * or the system is shutting down with i2c_set_atomic_mode). */
int i2c_in_atomic_xfer_mode(void);
void i2c_set_atomic_mode(int on);

/* ---- bus lock ------------------------------------------------------------------ */

void i2c_lock_bus(struct i2c_adapter *adap, unsigned int flags);
int i2c_trylock_bus(struct i2c_adapter *adap, unsigned int flags);
void i2c_unlock_bus(struct i2c_adapter *adap, unsigned int flags);
/* The default operations (the adapter's own lock), for lock_ops that
 * wrap them. */
void i2c_adapter_lock_bus(struct i2c_adapter *adap, unsigned int flags);
int i2c_adapter_trylock_bus(struct i2c_adapter *adap, unsigned int flags);
void i2c_adapter_unlock_bus(struct i2c_adapter *adap, unsigned int flags);
/* Does the calling task hold the bus lock. */
int i2c_bus_locked_by_me(const struct i2c_adapter *adap);

/* ---- recovery ------------------------------------------------------------------ */

/* Run the adapter's recovery: 0, -EBUSY (none, or SCL stuck), -errno.
 * Called with the bus lock held (by the controller driver from its
 * transfer path). */
int i2c_recover_bus(struct i2c_adapter *adap);
int i2c_generic_scl_recovery(struct i2c_adapter *adap);

/* ---- power management ------------------------------------------------------------ */

/* After this, transfers fail with -ESHUTDOWN (warned once) until
 * resumed; waits for a transfer in progress (takes the bus lock). */
void i2c_mark_adapter_suspended(struct i2c_adapter *adap);
void i2c_mark_adapter_resumed(struct i2c_adapter *adap);
static inline int i2c_adapter_is_suspended(const struct i2c_adapter *adap)
{
	return (__atomic_load_n(&adap->locked_flags, __ATOMIC_ACQUIRE) >>
		I2C_ALF_IS_SUSPENDED) &
	       1;
}
/* System suspend: every bound client's suspend() (wake interrupts armed
 * for I2C_CLIENT_WAKE clients), then every adapter's pm->suspend; resume
 * in the reverse order.  0 or the first error (suspend is undone then).
 * May sleep. */
int i2c_pm_suspend_all(void);
int i2c_pm_resume_all(void);
/* The whole system sleep chain without a sleep state (there is none to
 * enter): I2C clients and adapters suspended, then the GPIO/pin
 * controllers, `ms' milliseconds asleep, everything resumed in the
 * reverse order.  One run at a time (-EBUSY).  0 or the first error.
 * Written to /proc/i2c as "suspend-test [ms]" by root.  May sleep. */
int i2c_pm_suspend_test(unsigned int ms);
/* System shutdown: clients' shutdown() (interrupts disabled after it),
 * then the adapters'.  May sleep. */
void i2c_shutdown_all(void);
/* The clients of one adapter only (a controller driver's own shutdown). */
void i2c_shutdown_adapter_clients(struct i2c_adapter *adap);

/* ---- SMBus (i2c-core-smbus.c) ------------------------------------------------- */

/* CRC-8 (polynomial x^8 + x^2 + x + 1) continuing from `crc'. */
uint8_t i2c_smbus_pec(uint8_t crc, const uint8_t *p, size_t count);
/* One SMBus transaction: 0 or -errno.  Takes the bus lock. */
int i2c_smbus_xfer(struct i2c_adapter *adap, uint16_t addr,
		   unsigned short flags, char read_write, uint8_t command,
		   int protocol, union i2c_smbus_data *data);
int __i2c_smbus_xfer(struct i2c_adapter *adap, uint16_t addr,
		     unsigned short flags, char read_write, uint8_t command,
		     int protocol, union i2c_smbus_data *data);
/* The value (>= 0) or -errno. */
int i2c_smbus_read_byte(const struct i2c_client *client);
int i2c_smbus_write_byte(const struct i2c_client *client, uint8_t value);
int i2c_smbus_read_byte_data(const struct i2c_client *client,
			     uint8_t command);
int i2c_smbus_write_byte_data(const struct i2c_client *client,
			      uint8_t command, uint8_t value);
int i2c_smbus_read_word_data(const struct i2c_client *client,
			     uint8_t command);
int i2c_smbus_write_word_data(const struct i2c_client *client,
			      uint8_t command, uint16_t value);
int i2c_smbus_read_word_swapped(const struct i2c_client *client,
				uint8_t command);
int i2c_smbus_write_word_swapped(const struct i2c_client *client,
				 uint8_t command, uint16_t value);
/* Block reads return the length (bytes stored in `values', which must
 * hold I2C_SMBUS_BLOCK_MAX). */
int i2c_smbus_read_block_data(const struct i2c_client *client,
			      uint8_t command, uint8_t *values);
int i2c_smbus_write_block_data(const struct i2c_client *client,
			       uint8_t command, uint8_t length,
			       const uint8_t *values);
int i2c_smbus_read_i2c_block_data(const struct i2c_client *client,
				  uint8_t command, uint8_t length,
				  uint8_t *values);
int i2c_smbus_write_i2c_block_data(const struct i2c_client *client,
				   uint8_t command, uint8_t length,
				   const uint8_t *values);
/* I2C block read of `length' bytes, emulated with smaller reads when the
 * adapter cannot do I2C block reads. */
int i2c_smbus_read_i2c_block_data_or_emulated(const struct i2c_client *client,
					      uint8_t command, uint8_t length,
					      uint8_t *values);

/* ---- ACPI (i2c-core-acpi.c) ---------------------------------------------------- */

/* The speed of the slowest I2cSerialBus connection to the adapter whose
 * ACPI device is `adapter_handle' (with the known firmware fixups), 0
 * when no device names that adapter.  May sleep. */
uint32_t i2c_acpi_find_bus_speed(void *adapter_handle);
/* The interrupt of an ACPI client as probe resolves it: > 0, -ENOENT
 * (none), -EPROBE_DEFER, -errno.  *wake (may be NULL): wake capable. */
int i2c_acpi_get_irq(struct i2c_client *client, int *wake);
/* Forget the cached scan of I2cSerialBus devices (tests; firmware table
 * changes). */
void i2c_acpi_invalidate_cache(void);
/* Recovery GPIOs described by the adapter's ACPI device (_DSD
 * "scl-gpios" / "sda-gpios"), requested for bus recovery: 0 with *scl
 * set (*sda when present, else NULL), -ENOENT none described,
 * -EPROBE_DEFER, -errno.  May sleep. */
int i2c_acpi_get_recovery_gpios(struct i2c_adapter *adap,
				struct gpio_desc **scl, struct gpio_desc **sda);

/* ---- initialisation and diagnostics ------------------------------------------- */

/* The core (tables, /dev/i2c-N major).  Once, process context. */
void i2c_core_init(void);
/* End of the boot-time I2C initialisation: retry deferred probes, log a
 * summary. */
void i2c_core_late_init(void);
/* /proc/i2c (adapters, speeds, clients, counters) and /proc/bus/i2c
 * (the bus list in the format bus listing tools read). */
long i2c_proc_show(struct pfs_node *n, char *buf, long cap);
long i2c_proc_bus_show(struct pfs_node *n, char *buf, long cap);
/* Commands written to /proc/i2c (root): "suspend-test [ms]". */
long i2c_proc_store(struct pfs_node *n, const char *buf, long len);
long i2c_core_show(char *buf, long cap);

/* Boot-time entry points of the controller drivers (kernel/dev/i2c/
 * busses): the DesignWare PCI driver and ACPI controllers. */
void i2c_designware_init(void);

#endif /* KERNEL_DEV_I2C_I2C_H */
