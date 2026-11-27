// LikeOS I2C - internals shared by the core's files
//
// i2c-core-base.c (adapters, clients, drivers, transfers, locking,
// recovery, power, /proc), i2c-core-acpi.c (ACPI enumeration, bus speed,
// client interrupts, the GenericSerialBus operation region),
// i2c-core-smbus.c (SMBus protocols over I2C) and i2c-dev.c (/dev/i2c-N)
// share these.  Nothing outside kernel/dev/i2c includes this header.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_I2C_I2C_CORE_H
#define KERNEL_DEV_I2C_I2C_CORE_H

#include <kernel/dev/i2c/i2c.h>

/* The registry lock: adapter table, driver table, client lists.  Never
 * held across a driver callback or a transfer. */
extern struct mutex g_i2c_core_lock;

/* Adapter table access under g_i2c_core_lock. */
struct i2c_adapter *i2c_adapter_slot_locked(int nr);

/* Is `addr' (with I2C_CLIENT_TEN in flags) used by a client of `adap'.
 * With `bound_only', only clients with a driver count (the /dev/i2c-N
 * rule).  Takes g_i2c_core_lock.  0 or -EBUSY. */
int i2c_check_addr_busy(struct i2c_adapter *adap, unsigned int addr,
			unsigned short flags, int bound_only);

/* Take the bus lock for a transfer: sleeping normally; in atomic mode
 * only tried (-EAGAIN when busy).  0 or -EAGAIN. */
int __i2c_lock_bus_helper(struct i2c_adapter *adap);
/* -ESHUTDOWN (warned once per suspend) when the adapter is suspended. */
int __i2c_check_suspended(struct i2c_adapter *adap);

/* Transfer statistics. */
void i2c_stat_add(uint64_t *ctr, uint64_t n);

/* Format into a /proc buffer: `pos' advanced by the full length (bytes
 * beyond `cap' are dropped), as the pseudo filesystem's printf does. */
long i2c_show_printf(char *buf, long cap, long pos, const char *fmt, ...)
	__attribute__((format(printf, 4, 5)));

/* Messages of a recurring condition: a burst of I2C_RL_BURST per
 * I2C_RL_INTERVAL_NS window, the number left out reported with the next
 * one let through.  The caller serialises calls on one state (the bus
 * lock, typically). */
#define I2C_RL_BURST 10
#define I2C_RL_INTERVAL_NS (5ull * 1000000000ull)
struct i2c_ratelimit {
	uint64_t start_ns;
	unsigned int count;
	unsigned int missed;
};
/* 1: print now (*missed, when not NULL, gets the count left out since the
 * last one printed), 0: leave this one out. */
int i2c_ratelimit_ok(struct i2c_ratelimit *rl, unsigned int *missed);

/* Bounded string copy, always terminated. */
void i2c_strlcpy(char *dst, const char *src, size_t cap);

/* ---- i2c-core-acpi.c ------------------------------------------------------- */

/* Create the clients whose I2cSerialBus names the adapter (after the
 * adapter is in the table, before drivers are bound).  May sleep. */
void i2c_acpi_register_devices(struct i2c_adapter *adap);
/* Is `adev' on a board whose tables describe bogus I2C devices, and not
 * one of the ids known to be right there (no client is made then). */
int i2c_acpi_quirk_skip_client_enumeration(const struct acpi_device *adev);
/* AML access to the adapter's devices through GenericSerialBus
 * operation regions declared under the adapter's ACPI device. */
int i2c_acpi_install_space_handler(struct i2c_adapter *adap);
void i2c_acpi_remove_space_handler(struct i2c_adapter *adap);
/* Put a client's ACPI device in D0 for probe / back to low power. */
void i2c_acpi_client_power(struct i2c_client *client, int on);
/* May the client be probed without powering it up first (driver flag +
 * firmware property "i2c-allow-low-power-probe"). */
int i2c_acpi_waive_d0_probe(struct i2c_client *client,
			    const struct i2c_driver *drv);
/* Release what i2c_acpi_get_irq() created for the client. */
void i2c_acpi_put_irq(struct i2c_client *client);
/* Clients of `adap' are being removed: forget that their ACPI devices
 * were enumerated, so a new adapter may create them again. */
void i2c_acpi_forget_enumerated(struct i2c_client *client);

/* ---- i2c-dev.c -------------------------------------------------------------------- */

#define I2C_DEV_MAJOR 89

int i2c_dev_init(void);
/* /dev/i2c-N appears / goes (adapter registration / deletion). */
int i2c_dev_attach_adapter(struct i2c_adapter *adap);
void i2c_dev_detach_adapter(struct i2c_adapter *adap);
/* The node's memory goes, once nothing can reach it any more (called
 * after every i2c_get_adapter reference is gone). */
void i2c_dev_free_adapter(struct i2c_adapter *adap);

#endif /* KERNEL_DEV_I2C_I2C_CORE_H */
