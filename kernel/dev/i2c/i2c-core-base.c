// LikeOS I2C - core: adapters, clients, drivers, transfers, bus lock,
// bus recovery, power management, /proc
//
// Registry.  Adapters live in a table indexed by bus number, drivers in a
// table of slots, clients on their adapter's list; all three change under
// g_i2c_core_lock, a sleeping mutex that is never held across a driver
// callback or a transfer, so a probe() may itself create clients or run
// transfers.
//
// Object lifetime.
//   * An adapter is the controller driver's memory.  Its reference count
//     is 1 while registered plus one per i2c_get_adapter() user (an open
//     /dev/i2c-N file).  i2c_del_adapter() unregisters it (no new
//     references), removes its clients and the device node, and then
//     waits until only the registration's reference is left -- after it
//     returns the driver may free the adapter.
//   * A client is the core's (kcalloc).  The adapter's client list holds
//     one reference, dropped by i2c_unregister_device(); anybody else who
//     keeps the pointer takes its own (i2c_client_get).  It is freed with
//     its last reference.
//   * A driver is the driver's memory and must stay until i2c_del_driver()
//     returns; probes in flight are counted (drv->binding) and waited for.
//
// Binding.  i2c_try_bind() walks the driver table in registration order
// under the registry lock, takes the first driver that matches (ACPI ids,
// then the I2C id table by client name), drops the lock and probes with
// the client's bind lock held.  -EPROBE_DEFER marks the client deferred;
// every successful bind, adapter registration and driver registration
// retries the deferred clients (one runner at a time; a request made while
// one runs makes it go round once more).
//
// Transfers.  i2c_transfer() takes the bus lock (or, with interrupts off,
// only tries it), refuses transfers to a suspended adapter (-ESHUTDOWN,
// warned once) and transfers the adapter's quirks rule out (-EOPNOTSUPP),
// and retries a transfer that lost arbitration (-EAGAIN) up to
// adapter->retries times while adapter->timeout has not passed.
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) 1995-99 Simon G. Vogl, 2013-2017 Wolfram Sang

#include <kernel/dev/i2c/i2c-core.h>
#include <kernel/dev/gpio/gpiolib.h>
#include <kernel/hal/acpi_dev.h>
#include <kernel/ke/irq.h>
#include <kernel/ke/delay.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/ke/cred.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

DEFINE_MUTEX(g_i2c_core_lock);
static struct i2c_adapter *g_adapters[I2C_MAX_ADAPTERS];
static struct i2c_driver *g_drivers[I2C_MAX_DRIVERS];
static int g_core_ready;
static volatile int g_atomic_mode;
/* Probes in flight finished (i2c_del_driver waits on it). */
static DECLARE_WAIT_QUEUE_HEAD(g_bind_wq);
/* The deferred-probe runner. */
static spinlock_t g_retry_lock = SPINLOCK_INIT("i2c_retry");
static int g_retry_running, g_retry_pending;

/* A log line at most ten times per call site (driver errors that a
 * misbehaving device can repeat endlessly). */
#define i2c_log_ratelimited(fmt, ...)                                       \
	do {                                                                \
		static unsigned int __i2c_rl;                               \
		unsigned int __n = __atomic_add_fetch(&__i2c_rl, 1,         \
						      __ATOMIC_RELAXED);    \
		if (__n <= 10)                                              \
			kprintf(fmt "%s", ##__VA_ARGS__,                    \
				__n == 10 ? " (further ones suppressed)\n" : \
					    "\n");                          \
	} while (0)

/* ==========================================================================
 * Small helpers
 * ========================================================================== */

int i2c_ratelimit_ok(struct i2c_ratelimit *rl, unsigned int *missed)
{
	uint64_t now = hrtimer_now_ns();

	if (!rl->start_ns || now - rl->start_ns > I2C_RL_INTERVAL_NS) {
		rl->start_ns = now ? now : 1;
		rl->count = 0;
	}
	if (rl->count >= I2C_RL_BURST) {
		rl->missed++;
		return 0;
	}
	rl->count++;
	if (missed)
		*missed = rl->missed;
	rl->missed = 0;
	return 1;
}

void i2c_strlcpy(char *dst, const char *src, size_t cap)
{
	size_t i = 0;

	if (!cap)
		return;
	if (src)
		for (; i + 1 < cap && src[i]; i++)
			dst[i] = src[i];
	dst[i] = '\0';
}

long i2c_show_printf(char *buf, long cap, long pos, const char *fmt, ...)
{
	char tmp[256];
	__builtin_va_list ap;
	int n;

	__builtin_va_start(ap, fmt);
	n = kvsnprintf(tmp, sizeof(tmp), fmt, ap);
	__builtin_va_end(ap);
	if (n < 0)
		return pos;
	if (n >= (int)sizeof(tmp))
		n = (int)sizeof(tmp) - 1;
	for (int i = 0; i < n; i++)
		if (pos + i >= 0 && pos + i < cap)
			buf[pos + i] = tmp[i];
	return pos + n;
}

void i2c_stat_add(uint64_t *ctr, uint64_t n)
{
	__atomic_add_fetch(ctr, n, __ATOMIC_RELAXED);
}

static uint64_t i2c_now_ms(void)
{
	return hrtimer_now_ns() / 1000000ull;
}

static int test_and_set_flag(volatile unsigned long *flags, int bit)
{
	unsigned long m = 1ul << bit;

	return (__atomic_fetch_or(flags, m, __ATOMIC_ACQ_REL) & m) != 0;
}

static void set_flag(volatile unsigned long *flags, int bit)
{
	__atomic_fetch_or(flags, 1ul << bit, __ATOMIC_ACQ_REL);
}

static void clear_flag(volatile unsigned long *flags, int bit)
{
	__atomic_fetch_and(flags, ~(1ul << bit), __ATOMIC_ACQ_REL);
}

static int test_flag(const volatile unsigned long *flags, int bit)
{
	return (__atomic_load_n(flags, __ATOMIC_ACQUIRE) >> bit) & 1;
}

struct i2c_adapter *i2c_adapter_slot_locked(int nr)
{
	if (nr < 0 || nr >= I2C_MAX_ADAPTERS)
		return NULL;
	return g_adapters[nr];
}

/* ==========================================================================
 * Addresses
 * ========================================================================== */

int i2c_check_addr_validity(unsigned int addr, unsigned short flags)
{
	if (flags & I2C_CLIENT_TEN) {
		/* 10-bit address, all values are valid */
		if (addr > 0x3ff)
			return -EINVAL;
	} else {
		/* 7-bit address, reject the general call address */
		if (addr == 0x00 || addr > 0x7f)
			return -EINVAL;
	}
	return 0;
}

int i2c_check_7bit_addr_validity_strict(unsigned short addr)
{
	/* Reserved by the I2C specification:
	 *   0x00       general call / START byte
	 *   0x01       CBUS
	 *   0x02       a different bus format
	 *   0x03       future purposes
	 *   0x04-0x07  high-speed master codes
	 *   0x78-0x7b  10-bit addressing
	 *   0x7c-0x7f  future purposes */
	if (addr < 0x08 || addr > 0x77)
		return -EINVAL;
	return 0;
}

static unsigned int i2c_encode_addr(unsigned int addr, unsigned short flags)
{
	if (flags & I2C_CLIENT_TEN)
		addr |= I2C_ADDR_OFFSET_TEN_BIT;
	if (flags & I2C_CLIENT_SLAVE)
		addr |= I2C_ADDR_OFFSET_SLAVE;
	return addr;
}

static int i2c_check_addr_busy_locked(struct i2c_adapter *adap,
				      unsigned int addr, unsigned short flags,
				      int bound_only)
{
	struct i2c_client *c;
	unsigned int want = i2c_encode_addr(addr, flags);

	list_for_each_entry(c, &adap->clients, node) {
		if (c->dead)
			continue;
		if (bound_only) {
			/* /dev/i2c-N compares the plain address and only
			 * minds clients that a driver owns. */
			if (c->addr == addr && c->driver)
				return -EBUSY;
		} else if (i2c_encode_addr(c->addr, c->flags) == want) {
			return -EBUSY;
		}
	}
	return 0;
}

int i2c_check_addr_busy(struct i2c_adapter *adap, unsigned int addr,
			unsigned short flags, int bound_only)
{
	int ret;

	mutex_lock(&g_i2c_core_lock);
	ret = i2c_check_addr_busy_locked(adap, addr, flags, bound_only);
	mutex_unlock(&g_i2c_core_lock);
	return ret;
}

/* ==========================================================================
 * Bus lock
 * ========================================================================== */

void i2c_adapter_lock_bus(struct i2c_adapter *adap, unsigned int flags)
{
	(void)flags; /* no multiplexers: root adapter == segment */
	mutex_lock(&adap->bus_lock);
	adap->bus_lock_owner = sched_current();
}

int i2c_adapter_trylock_bus(struct i2c_adapter *adap, unsigned int flags)
{
	(void)flags;
	if (!mutex_trylock(&adap->bus_lock))
		return 0;
	adap->bus_lock_owner = sched_current();
	return 1;
}

void i2c_adapter_unlock_bus(struct i2c_adapter *adap, unsigned int flags)
{
	(void)flags;
	WARN_ON_ONCE(adap->bus_lock_owner != (void *)sched_current());
	adap->bus_lock_owner = NULL;
	mutex_unlock(&adap->bus_lock);
}

void i2c_lock_bus(struct i2c_adapter *adap, unsigned int flags)
{
	if (adap->lock_ops)
		adap->lock_ops->lock_bus(adap, flags);
	else
		i2c_adapter_lock_bus(adap, flags);
}

int i2c_trylock_bus(struct i2c_adapter *adap, unsigned int flags)
{
	if (adap->lock_ops)
		return adap->lock_ops->trylock_bus(adap, flags);
	return i2c_adapter_trylock_bus(adap, flags);
}

void i2c_unlock_bus(struct i2c_adapter *adap, unsigned int flags)
{
	if (adap->lock_ops)
		adap->lock_ops->unlock_bus(adap, flags);
	else
		i2c_adapter_unlock_bus(adap, flags);
}

int i2c_bus_locked_by_me(const struct i2c_adapter *adap)
{
	return mutex_is_locked(&adap->bus_lock) &&
	       adap->bus_lock_owner == (void *)sched_current();
}

int i2c_in_atomic_xfer_mode(void)
{
	return __atomic_load_n(&g_atomic_mode, __ATOMIC_ACQUIRE) ||
	       irqs_disabled();
}

void i2c_set_atomic_mode(int on)
{
	__atomic_store_n(&g_atomic_mode, on ? 1 : 0, __ATOMIC_RELEASE);
}

int __i2c_lock_bus_helper(struct i2c_adapter *adap)
{
	if (i2c_in_atomic_xfer_mode()) {
		WARN(!adap->algo->master_xfer_atomic &&
			     !adap->algo->smbus_xfer_atomic,
		     "No atomic I2C transfer handler for '%s'",
		     adap->dev_name);
		return i2c_trylock_bus(adap, I2C_LOCK_SEGMENT) ? 0 : -EAGAIN;
	}
	might_sleep();
	i2c_lock_bus(adap, I2C_LOCK_SEGMENT);
	return 0;
}

int __i2c_check_suspended(struct i2c_adapter *adap)
{
	if (test_flag(&adap->locked_flags, I2C_ALF_IS_SUSPENDED)) {
		i2c_stat_add(&adap->stats.rejected_suspended, 1);
		if (!test_and_set_flag(&adap->locked_flags,
				       I2C_ALF_SUSPEND_REPORTED))
			WARN(1, "i2c %s: transfer while suspended",
			     adap->dev_name);
		return -ESHUTDOWN;
	}
	return 0;
}

/* ==========================================================================
 * Transfers
 * ========================================================================== */

static int i2c_quirk_error(struct i2c_adapter *adap, struct i2c_msg *msg,
			   const char *err_msg)
{
	i2c_stat_add(&adap->stats.rejected_quirk, 1);
	i2c_log_ratelimited("i2c %s: adapter quirk: %s (addr 0x%04x, size %u, %s)",
			    adap->dev_name, err_msg, msg->addr, msg->len,
			    (msg->flags & I2C_M_RD) ? "read" : "write");
	return -EOPNOTSUPP;
}

static int i2c_quirk_exceeded(uint16_t val, uint16_t max)
{
	return max && val > max;
}

static int i2c_check_for_quirks(struct i2c_adapter *adap,
				struct i2c_msg *msgs, int num)
{
	const struct i2c_adapter_quirks *q = adap->quirks;
	int max_num = q->max_num_msgs, i;
	int do_len_check = 1;

	if (q->flags & I2C_AQ_COMB) {
		max_num = 2;

		/* special checks for combined messages */
		if (num == 2) {
			if ((q->flags & I2C_AQ_COMB_WRITE_FIRST) &&
			    (msgs[0].flags & I2C_M_RD))
				return i2c_quirk_error(adap, &msgs[0],
						       "1st comb msg must be write");
			if ((q->flags & I2C_AQ_COMB_READ_SECOND) &&
			    !(msgs[1].flags & I2C_M_RD))
				return i2c_quirk_error(adap, &msgs[1],
						       "2nd comb msg must be read");
			if ((q->flags & I2C_AQ_COMB_SAME_ADDR) &&
			    msgs[0].addr != msgs[1].addr)
				return i2c_quirk_error(adap, &msgs[0],
						       "comb msg only to same addr");
			if (i2c_quirk_exceeded(msgs[0].len,
					       q->max_comb_1st_msg_len))
				return i2c_quirk_error(adap, &msgs[0],
						       "msg too long");
			if (i2c_quirk_exceeded(msgs[1].len,
					       q->max_comb_2nd_msg_len))
				return i2c_quirk_error(adap, &msgs[1],
						       "msg too long");
			do_len_check = 0;
		}
	}

	if (max_num && num > max_num)
		return i2c_quirk_error(adap, &msgs[0], "too many messages");

	for (i = 0; i < num; i++) {
		uint16_t len = msgs[i].len;

		if (msgs[i].flags & I2C_M_RD) {
			if (do_len_check &&
			    i2c_quirk_exceeded(len, q->max_read_len))
				return i2c_quirk_error(adap, &msgs[i],
						       "msg too long");
			if ((q->flags & I2C_AQ_NO_ZERO_LEN_READ) && len == 0)
				return i2c_quirk_error(adap, &msgs[i],
						       "no zero length");
		} else {
			if (do_len_check &&
			    i2c_quirk_exceeded(len, q->max_write_len))
				return i2c_quirk_error(adap, &msgs[i],
						       "msg too long");
			if ((q->flags & I2C_AQ_NO_ZERO_LEN_WRITE) && len == 0)
				return i2c_quirk_error(adap, &msgs[i],
						       "no zero length");
		}
	}
	return 0;
}

/* Arguments a driver must never see: a message with data but no buffer,
 * an address its own flags rule out. */
static int i2c_check_msgs(struct i2c_adapter *adap, struct i2c_msg *msgs,
			  int num)
{
	for (int i = 0; i < num; i++) {
		if (WARN(msgs[i].len && !msgs[i].buf,
			 "i2c %s: message %d of %d bytes has no buffer",
			 adap->dev_name, i, msgs[i].len))
			return -EINVAL;
		if ((msgs[i].flags & I2C_M_TEN) ? msgs[i].addr > 0x3ff :
						  msgs[i].addr > 0x7f) {
			i2c_log_ratelimited("i2c %s: invalid %s address 0x%x in message %d",
					    adap->dev_name,
					    (msgs[i].flags & I2C_M_TEN) ?
						    "10-bit" :
						    "7-bit",
					    msgs[i].addr, i);
			return -EINVAL;
		}
	}
	return 0;
}

static void i2c_account_result(struct i2c_adapter *adap, int ret)
{
	if (ret >= 0)
		i2c_stat_add(&adap->stats.ok, 1);
	else if (ret == -ENXIO || ret == -EREMOTEIO)
		i2c_stat_add(&adap->stats.err_nack, 1);
	else if (ret == -EAGAIN)
		i2c_stat_add(&adap->stats.err_arb, 1);
	else if (ret == -ETIMEDOUT)
		i2c_stat_add(&adap->stats.err_timeout, 1);
	else
		i2c_stat_add(&adap->stats.err_other, 1);
}

int __i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	uint64_t orig_ms, bytes = 0;
	int ret, try;

	if (!adap->algo->master_xfer)
		return -EOPNOTSUPP;
	if (WARN_ON(!msgs || num < 1))
		return -EINVAL;
	/* The bus lock is the caller's (i2c_transfer, SMBus, i2c-dev). */
	lockdep_assert_held(&adap->bus_lock);

	ret = __i2c_check_suspended(adap);
	if (ret)
		return ret;
	if (adap->quirks && i2c_check_for_quirks(adap, msgs, num))
		return -EOPNOTSUPP;
	ret = i2c_check_msgs(adap, msgs, num);
	if (ret)
		return ret;

	for (int i = 0; i < num; i++)
		bytes += msgs[i].len;
	i2c_stat_add(&adap->stats.xfers, 1);
	i2c_stat_add(&adap->stats.msgs, (uint64_t)num);
	i2c_stat_add(&adap->stats.bytes, bytes);

	/* Retry automatically on arbitration loss. */
	orig_ms = i2c_now_ms();
	for (ret = 0, try = 0; try <= adap->retries; try++) {
		if (i2c_in_atomic_xfer_mode() && adap->algo->master_xfer_atomic)
			ret = adap->algo->master_xfer_atomic(adap, msgs, num);
		else
			ret = adap->algo->master_xfer(adap, msgs, num);

		if (ret != -EAGAIN)
			break;
		if (i2c_now_ms() - orig_ms > (uint64_t)adap->timeout)
			break;
		if (try < adap->retries)
			i2c_stat_add(&adap->stats.retries, 1);
	}
	WARN_ON_ONCE(ret > num);
	i2c_account_result(adap, ret);
	return ret;
}

int i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	int ret;

	if (WARN_ON(!adap))
		return -EINVAL;
	ret = __i2c_lock_bus_helper(adap);
	if (ret)
		return ret;
	ret = __i2c_transfer(adap, msgs, num);
	i2c_unlock_bus(adap, I2C_LOCK_SEGMENT);
	return ret;
}

int i2c_transfer_buffer_flags(const struct i2c_client *client, char *buf,
			      int count, uint16_t flags)
{
	struct i2c_msg msg;
	int ret;

	if (WARN_ON(!client || !client->adapter))
		return -EINVAL;
	/* A message carries at most 65535 bytes. */
	if (count < 0 || count > 0xffff)
		return -EINVAL;
	msg.addr = client->addr;
	msg.flags = (uint16_t)(flags | (client->flags & I2C_M_TEN));
	msg.len = (uint16_t)count;
	msg.buf = (uint8_t *)buf;
	ret = i2c_transfer(client->adapter, &msg, 1);
	/* One message transferred: the byte count, else the error. */
	return (ret == 1) ? count : ret;
}

uint32_t i2c_get_functionality(struct i2c_adapter *adap)
{
	if (!adap || !adap->algo || !adap->algo->functionality)
		return 0;
	return adap->algo->functionality(adap);
}

/* ==========================================================================
 * Bus recovery
 * ========================================================================== */

/* Clock pulses at 100 kHz: 5 us per level. */
#define RECOVERY_DELAY_US 5
#define RECOVERY_HALF_DELAY_US 3
#define RECOVERY_CLK_CNT 9

static int get_scl_gpio_value(struct i2c_adapter *adap)
{
	return gpiod_get_value_cansleep(adap->bus_recovery_info->scl_gpiod);
}

static void set_scl_gpio_value(struct i2c_adapter *adap, int val)
{
	gpiod_set_value_cansleep(adap->bus_recovery_info->scl_gpiod, val);
}

static int get_sda_gpio_value(struct i2c_adapter *adap)
{
	return gpiod_get_value_cansleep(adap->bus_recovery_info->sda_gpiod);
}

static void set_sda_gpio_value(struct i2c_adapter *adap, int val)
{
	gpiod_set_value_cansleep(adap->bus_recovery_info->sda_gpiod, val);
}

static int i2c_generic_bus_free(struct i2c_adapter *adap)
{
	struct i2c_bus_recovery_info *bri = adap->bus_recovery_info;
	int ret = -EOPNOTSUPP;

	if (bri->get_bus_free)
		ret = bri->get_bus_free(adap);
	else if (bri->get_sda)
		ret = bri->get_sda(adap);
	if (ret < 0)
		return ret;
	return ret ? 0 : -EBUSY;
}

int i2c_generic_scl_recovery(struct i2c_adapter *adap)
{
	struct i2c_bus_recovery_info *bri = adap->bus_recovery_info;
	int i = 0, scl = 1, ret = 0;

	if (bri->prepare_recovery)
		bri->prepare_recovery(adap);

	/* With control over SDA, every pulse also ends in a STOP (SDA
	 * follows SCL half a cycle later), so extra pulses do no harm.
	 * tSU;STO is 4 us; 5 us is used. */
	bri->set_scl(adap, scl);
	udelay(RECOVERY_DELAY_US);
	if (bri->set_sda)
		bri->set_sda(adap, scl);
	udelay(RECOVERY_HALF_DELAY_US);

	/* SCL is high now: nine falling and rising edges follow. */
	while (i++ < RECOVERY_CLK_CNT * 2) {
		if (scl) {
			/* SCL shouldn't be low here */
			if (!bri->get_scl(adap)) {
				kprintf("i2c %s: SCL is stuck low, exit recovery\n",
					adap->dev_name);
				ret = -EBUSY;
				break;
			}
		}

		scl = !scl;
		bri->set_scl(adap, scl);
		/* the STOP again */
		if (scl)
			udelay(RECOVERY_DELAY_US); /* tSU;STO */
		else
			udelay(RECOVERY_HALF_DELAY_US); /* tF, tHD;DAT */
		if (bri->set_sda)
			bri->set_sda(adap, scl);
		udelay(RECOVERY_HALF_DELAY_US);

		if (scl) {
			ret = i2c_generic_bus_free(adap);
			if (ret == 0)
				break;
		}
	}

	/* Without a way to look at the bus, assume it worked. */
	if (ret == -EOPNOTSUPP)
		ret = 0;

	if (bri->unprepare_recovery)
		bri->unprepare_recovery(adap);
	return ret;
}

int i2c_recover_bus(struct i2c_adapter *adap)
{
	int ret;

	if (!adap->bus_recovery_info || !adap->bus_recovery_info->recover_bus)
		return -EBUSY;
	i2c_stat_add(&adap->stats.recoveries, 1);
	ret = adap->bus_recovery_info->recover_bus(adap);
	if (ret) {
		i2c_stat_add(&adap->stats.recovery_failed, 1);
		i2c_log_ratelimited("i2c %s: bus recovery failed (%d)",
				    adap->dev_name, ret);
	}
	return ret;
}

/* Set up the adapter's recovery: look up recovery GPIOs the firmware
 * describes when the driver relies on the generic method, check that the
 * method has what it needs.  -EPROBE_DEFER when the GPIO controller is
 * not there yet; other problems only drop recovery. */
static int i2c_init_recovery(struct i2c_adapter *adap)
{
	struct i2c_bus_recovery_info *bri = adap->bus_recovery_info;
	const char *err_str;
	int is_error_level = 1;

	if (!bri)
		return 0;

	if ((!bri->recover_bus || bri->recover_bus == i2c_generic_scl_recovery) &&
	    !bri->scl_gpiod) {
		struct gpio_desc *scl = NULL, *sda = NULL;
		int rc = i2c_acpi_get_recovery_gpios(adap, &scl, &sda);

		if (rc == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		if (!rc && scl) {
			bri->scl_gpiod = scl;
			bri->sda_gpiod = sda;
			bri->recover_bus = i2c_generic_scl_recovery;
			kprintf("i2c %s: using generic GPIOs for recovery\n",
				adap->dev_name);
		}
	}

	if (!bri->recover_bus) {
		err_str = "no suitable method provided";
		is_error_level = 0;
		goto err;
	}

	if (bri->scl_gpiod && bri->recover_bus == i2c_generic_scl_recovery) {
		bri->get_scl = get_scl_gpio_value;
		bri->set_scl = set_scl_gpio_value;
		if (bri->sda_gpiod) {
			bri->get_sda = get_sda_gpio_value;
			if (gpiod_get_direction(bri->sda_gpiod) ==
			    GPIO_LINE_DIRECTION_OUT)
				bri->set_sda = set_sda_gpio_value;
		}
	} else if (bri->recover_bus == i2c_generic_scl_recovery) {
		/* Generic SCL recovery through driver accessors. */
		if (!bri->set_scl || !bri->get_scl) {
			err_str = "no {get|set}_scl() found";
			goto err;
		}
		if (!bri->set_sda && !bri->get_sda) {
			err_str = "either get_sda() or set_sda() needed";
			goto err;
		}
	}
	return 0;
err:
	if (is_error_level)
		kprintf("i2c %s: Not using recovery: %s\n", adap->dev_name,
			err_str);
	adap->bus_recovery_info = NULL;
	return -EINVAL;
}

/* ==========================================================================
 * Clients
 * ========================================================================== */

static void i2c_client_release(struct i2c_client *client)
{
	WARN_ON(client->driver);
	if (client->acpi_dev)
		acpi_dev_put(client->acpi_dev);
	client->acpi_dev = NULL;
	client->magic = 0;
	kfree(client);
}

struct i2c_client *i2c_client_get(struct i2c_client *client)
{
	if (client)
		refcount_inc(&client->ref);
	return client;
}

void i2c_client_put(struct i2c_client *client)
{
	if (client && refcount_dec_and_test(&client->ref))
		i2c_client_release(client);
}

struct i2c_client *i2c_verify_client(void *ptr)
{
	struct i2c_client *c = ptr;

	if (!c || I2C_IS_ERR(c) || c->magic != I2C_CLIENT_MAGIC || c->dead)
		return NULL;
	return c;
}

unsigned long i2c_get_match_data(const struct i2c_client *client)
{
	if (!client)
		return 0;
	if (client->acpi_id)
		return client->acpi_id->driver_data;
	if (client->id)
		return client->id->driver_data;
	return 0;
}

/* "SSSS:BB:DD.F" -- a PCI function name. */
static int i2c_is_pci_name(const char *s)
{
	static const char pat[] = "xxxx:xx:xx.x";

	for (int i = 0; pat[i]; i++) {
		char c = s[i];

		if (pat[i] == 'x') {
			if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
			      (c >= 'A' && c <= 'F')))
				return 0;
		} else if (c != pat[i]) {
			return 0;
		}
	}
	return s[12] == 0;
}

int i2c_client_devpath(const struct i2c_client *client, char *buf, size_t len)
{
	const struct i2c_adapter *adap;
	const char *dev = NULL;
	char drv[24];
	int n;

	if (!client || !client->adapter || !buf || !len)
		return -EINVAL;
	adap = client->adapter;
	/* parent_name is "<driver> <device>". */
	drv[0] = 0;
	for (size_t i = 0; adap->parent_name[i]; i++) {
		if (adap->parent_name[i] == ' ') {
			size_t k = i < sizeof(drv) - 1 ? i : sizeof(drv) - 1;

			kmemcpy(drv, adap->parent_name, k);
			drv[k] = 0;
			dev = &adap->parent_name[i + 1];
			break;
		}
	}
	if (dev && i2c_is_pci_name(dev))
		/* The controller's PCI function, the platform device its PCI
		 * glue creates (numbered like the bus), the bus, the client. */
		n = ksnprintf(buf, len, "/devices/pci%.4s:%.2s/%s/%s.%d/%s/%s",
			      dev, dev + 5, dev, drv, adap->nr, adap->dev_name,
			      client->dev_name);
	else if (dev && dev[0])
		/* A controller the firmware describes as a device of its own. */
		n = ksnprintf(buf, len, "/devices/platform/%s/%s/%s", dev,
			      adap->dev_name, client->dev_name);
	else
		n = ksnprintf(buf, len, "/devices/%s/%s", adap->dev_name,
			      client->dev_name);
	if (n < 0)
		return -EINVAL;
	if ((size_t)n >= len)
		return -ENAMETOOLONG;
	return 0;
}

struct i2c_client *i2c_find_client(struct i2c_adapter *adap, uint16_t addr)
{
	struct i2c_client *c, *found = NULL;

	mutex_lock(&g_i2c_core_lock);
	list_for_each_entry(c, &adap->clients, node) {
		if (!c->dead && c->addr == addr) {
			found = i2c_client_get(c);
			break;
		}
	}
	mutex_unlock(&g_i2c_core_lock);
	return found;
}

static const struct i2c_device_id *i2c_match_id(const struct i2c_device_id *id,
						const struct i2c_client *client)
{
	if (!id)
		return NULL;
	for (; id->name[0]; id++)
		if (!kstrcmp(client->name, id->name))
			return id;
	return NULL;
}

static const struct acpi_device_id *i2c_match_acpi(const struct i2c_client *client,
						   const struct i2c_driver *drv)
{
	if (!client->acpi_dev || !drv->acpi_match_table)
		return NULL;
	return acpi_match_device_ids(client->acpi_dev, drv->acpi_match_table);
}

/* Probe `client' with `drv' (bind lock held).  0, -EPROBE_DEFER, or the
 * driver's error; on any error the client is as before. */
static int i2c_device_probe(struct i2c_client *client, struct i2c_driver *drv,
			    const struct acpi_device_id *acpi_id,
			    const struct i2c_device_id *id)
{
	int status;

	might_sleep();
	mutex_assert_held(&client->bind_lock);
	BUG_ON(client->driver);

	client->irq = client->init_irq;
	if (!client->irq) {
		int irq = -ENOENT;

		if (client->flags & I2C_CLIENT_HOST_NOTIFY) {
			/* No adapter here raises SMBus host notify. */
			i2c_log_ratelimited("i2c %s: host notify interrupt not available",
					    client->dev_name);
		} else if (client->acpi_dev) {
			int wake = 0;

			irq = i2c_acpi_get_irq(client, &wake);
			if (irq > 0 && wake)
				client->flags |= I2C_CLIENT_WAKE;
		}
		if (irq == -EPROBE_DEFER) {
			status = irq;
			goto out_restore;
		}
		if (irq < 0)
			irq = 0;
		client->irq = irq;
	}

	/* An I2C id table is not needed when an ACPI table matched. */
	if (!drv->id_table && !acpi_id) {
		status = -ENODEV;
		goto out_restore;
	}

	if (client->acpi_dev && !i2c_acpi_waive_d0_probe(client, drv))
		i2c_acpi_client_power(client, 1);

	client->acpi_id = acpi_id;
	client->id = id;
	client->driver = drv;
	status = drv->probe ? drv->probe(client) : -EINVAL;
	if (!status)
		return 0;

	client->driver = NULL;
	client->acpi_id = NULL;
	client->id = NULL;
	if (client->powered)
		i2c_acpi_client_power(client, 0);
out_restore:
	i2c_acpi_put_irq(client);
	client->irq = client->init_irq;
	client->flags = (unsigned short)((client->flags & ~I2C_CLIENT_WAKE) |
					 (client->init_flags & I2C_CLIENT_WAKE));
	return status;
}

/* Undo a bind (bind lock held). */
static void i2c_device_remove(struct i2c_client *client)
{
	struct i2c_driver *drv = client->driver;

	mutex_assert_held(&client->bind_lock);
	if (!drv)
		return;
	if (drv->remove)
		drv->remove(client);
	if (client->wake_armed && client->irq > 0)
		disable_irq_wake(client->irq);
	client->wake_armed = 0;
	client->suspended = 0;
	if (client->powered)
		i2c_acpi_client_power(client, 0);
	i2c_acpi_put_irq(client);
	client->driver = NULL;
	client->acpi_id = NULL;
	client->id = NULL;
	client->driver_data = NULL;
	client->irq = client->init_irq;
	client->flags = (unsigned short)((client->flags & ~I2C_CLIENT_WAKE) |
					 (client->init_flags & I2C_CLIENT_WAKE));
}

/* Bind the first matching driver that accepts the client.  0 bound,
 * -EPROBE_DEFER, -ENODEV (none matched or all refused). */
static int i2c_try_bind(struct i2c_client *client)
{
	int ret = -ENODEV, slot = 0;

	/* A probe that registers clients itself (dummies for further
	 * addresses) leads back here through the deferred-probe retry:
	 * its own client is not bound again from inside its probe. */
	if (client->probing_task == (void *)sched_current())
		return -EBUSY;
	mutex_lock(&client->bind_lock);
	if (client->driver || client->dead)
		goto out;
	for (;;) {
		struct i2c_driver *drv = NULL;
		const struct acpi_device_id *acpi_id = NULL;
		const struct i2c_device_id *id = NULL;

		mutex_lock(&g_i2c_core_lock);
		for (; slot < I2C_MAX_DRIVERS; slot++) {
			struct i2c_driver *d = g_drivers[slot];

			if (!d || !d->registered)
				continue;
			acpi_id = i2c_match_acpi(client, d);
			id = i2c_match_id(d->id_table, client);
			if (acpi_id || id) {
				drv = d;
				__atomic_add_fetch(&drv->binding, 1,
						   __ATOMIC_ACQ_REL);
				break;
			}
		}
		mutex_unlock(&g_i2c_core_lock);
		if (!drv)
			break;

		client->probe_attempts++;
		client->probing_task = sched_current();
		ret = i2c_device_probe(client, drv, acpi_id, id);
		client->probing_task = NULL;
		client->probe_err = ret;

		mutex_lock(&g_i2c_core_lock);
		BUG_ON(drv->binding <= 0);
		__atomic_sub_fetch(&drv->binding, 1, __ATOMIC_ACQ_REL);
		mutex_unlock(&g_i2c_core_lock);
		wake_up(&g_bind_wq);

		if (!ret) {
			client->deferred = 0;
			kprintf("i2c %s: bound to %s, irq %d%s\n",
				client->dev_name, drv->name, client->irq,
				(client->flags & I2C_CLIENT_WAKE) ? " (wake)" :
								    "");
			break;
		}
		if (ret == -EPROBE_DEFER) {
			if (!client->deferred)
				kprintf("i2c %s: probe of %s deferred\n",
					client->dev_name, drv->name);
			client->deferred = 1;
			break;
		}
		if (ret != -ENODEV && ret != -ENXIO)
			kprintf("i2c %s: probe with driver %s failed with error %d\n",
				client->dev_name, drv->name, ret);
		slot++; /* the next matching driver may want it */
	}
out:
	mutex_unlock(&client->bind_lock);
	if (!ret)
		i2c_retry_deferred_probes();
	return ret;
}

static void i2c_unbind(struct i2c_client *client)
{
	mutex_lock(&client->bind_lock);
	i2c_device_remove(client);
	mutex_unlock(&client->bind_lock);
}

/* References to the clients of `adap' that pass `want' (the registry
 * lock held), into a kcalloc'd array; *n gets the count.  NULL with *n 0
 * when none (or out of memory). */
static struct i2c_client **
i2c_collect_clients(struct i2c_adapter *adap,
		    int (*want)(const struct i2c_client *c, void *arg),
		    void *arg, int *n)
{
	struct i2c_client *c, **arr;
	int count = 0, i = 0;

	*n = 0;
	mutex_lock(&g_i2c_core_lock);
	list_for_each_entry(c, &adap->clients, node)
		if (!c->dead && want(c, arg))
			count++;
	if (!count) {
		mutex_unlock(&g_i2c_core_lock);
		return NULL;
	}
	arr = kcalloc((size_t)count, sizeof(*arr));
	if (!arr) {
		mutex_unlock(&g_i2c_core_lock);
		kprintf("i2c %s: out of memory walking the clients\n",
			adap->dev_name);
		return NULL;
	}
	list_for_each_entry(c, &adap->clients, node)
		if (!c->dead && want(c, arg) && i < count)
			arr[i++] = i2c_client_get(c);
	mutex_unlock(&g_i2c_core_lock);
	*n = i;
	return arr;
}

static void i2c_put_clients(struct i2c_client **arr, int n)
{
	for (int i = 0; i < n; i++)
		i2c_client_put(arr[i]);
	kfree(arr);
}

/* A reference to every live adapter, in bus order (kcalloc'd). */
static struct i2c_adapter **i2c_collect_adapters(int *n)
{
	struct i2c_adapter **arr;
	int count = 0, i = 0;

	*n = 0;
	mutex_lock(&g_i2c_core_lock);
	for (int nr = 0; nr < I2C_MAX_ADAPTERS; nr++)
		if (g_adapters[nr] && g_adapters[nr]->registered &&
		    !g_adapters[nr]->dying)
			count++;
	if (!count) {
		mutex_unlock(&g_i2c_core_lock);
		return NULL;
	}
	arr = kcalloc((size_t)count, sizeof(*arr));
	if (!arr) {
		mutex_unlock(&g_i2c_core_lock);
		return NULL;
	}
	for (int nr = 0; nr < I2C_MAX_ADAPTERS && i < count; nr++) {
		struct i2c_adapter *a = g_adapters[nr];

		if (a && a->registered && !a->dying) {
			refcount_inc(&a->ref);
			arr[i++] = a;
		}
	}
	mutex_unlock(&g_i2c_core_lock);
	*n = i;
	return arr;
}

static void i2c_put_adapters(struct i2c_adapter **arr, int n)
{
	for (int i = 0; i < n; i++)
		i2c_put_adapter(arr[i]);
	kfree(arr);
}

static int want_unbound(const struct i2c_client *c, void *arg)
{
	int deferred_only = *(int *)arg;

	return !c->driver && (!deferred_only || c->deferred);
}

/* Try to bind the unbound (or only the deferred) clients of every
 * adapter. */
static void i2c_bind_all(int deferred_only)
{
	struct i2c_adapter **adaps;
	int na;

	adaps = i2c_collect_adapters(&na);
	for (int a = 0; a < na; a++) {
		struct i2c_client **cl;
		int nc;

		cl = i2c_collect_clients(adaps[a], want_unbound, &deferred_only,
					 &nc);
		for (int i = 0; i < nc; i++)
			(void)i2c_try_bind(cl[i]);
		if (cl)
			i2c_put_clients(cl, nc);
	}
	if (adaps)
		i2c_put_adapters(adaps, na);
}

void i2c_retry_deferred_probes(void)
{
	uint64_t fl;
	int again;

	if (!g_core_ready)
		return;
	spin_lock_irqsave(&g_retry_lock, &fl);
	if (g_retry_running) {
		/* The runner goes round once more. */
		g_retry_pending = 1;
		spin_unlock_irqrestore(&g_retry_lock, fl);
		return;
	}
	g_retry_running = 1;
	spin_unlock_irqrestore(&g_retry_lock, fl);

	do {
		spin_lock_irqsave(&g_retry_lock, &fl);
		g_retry_pending = 0;
		spin_unlock_irqrestore(&g_retry_lock, fl);

		i2c_bind_all(1);

		spin_lock_irqsave(&g_retry_lock, &fl);
		again = g_retry_pending;
		if (!again)
			g_retry_running = 0;
		spin_unlock_irqrestore(&g_retry_lock, fl);
	} while (again);
}

struct i2c_client *i2c_new_client_device(struct i2c_adapter *adap,
					 const struct i2c_board_info *info)
{
	struct i2c_client *client;
	int status;

	might_sleep();
	if (WARN_ON(!adap || !info))
		return I2C_ERR_PTR(-EINVAL);

	status = i2c_check_addr_validity(info->addr, info->flags);
	if (status) {
		kprintf("i2c %s: Invalid %d-bit I2C address 0x%02x\n",
			adap->dev_name, (info->flags & I2C_CLIENT_TEN) ? 10 : 7,
			info->addr);
		return I2C_ERR_PTR(status);
	}

	client = kcalloc(1, sizeof(*client));
	if (!client)
		return I2C_ERR_PTR(-ENOMEM);
	client->adapter = adap;
	client->flags = info->flags;
	client->init_flags = info->flags;
	client->addr = info->addr;
	client->init_irq = info->irq;
	client->irq = info->irq;
	i2c_strlcpy(client->name, info->type, sizeof(client->name));
	if (info->dev_name[0])
		i2c_strlcpy(client->dev_name, info->dev_name,
			    sizeof(client->dev_name));
	else
		ksnprintf(client->dev_name, sizeof(client->dev_name),
			  "%d-%04x", adap->nr,
			  i2c_encode_addr(info->addr, info->flags));
	client->magic = I2C_CLIENT_MAGIC;
	refcount_set(&client->ref, 1);
	INIT_LIST_HEAD(&client->node);
	mutex_init(&client->bind_lock, "i2c_client_bind");
	if (info->acpi_dev)
		client->acpi_dev = acpi_dev_get(info->acpi_dev);

	mutex_lock(&g_i2c_core_lock);
	if (!adap->registered || adap->dying) {
		status = -ENODEV;
	} else {
		status = i2c_check_addr_busy_locked(adap, info->addr,
						    info->flags, 0);
		if (!status)
			list_add_tail(&client->node, &adap->clients);
	}
	mutex_unlock(&g_i2c_core_lock);
	if (status) {
		kprintf("i2c %s: Failed to register i2c client %s at 0x%02x (%d)\n",
			adap->dev_name, client->name, client->addr, status);
		i2c_client_release(client);
		return I2C_ERR_PTR(status);
	}

	(void)i2c_try_bind(client);
	return client;
}

/* The driver of address-reserving dummy clients. */
static int dummy_probe(struct i2c_client *client)
{
	(void)client;
	return 0;
}

static const struct i2c_device_id g_dummy_id[] = {
	{ "dummy", 0 },
	{ "smbus_host_notify", 0 },
	{ "", 0 },
};

static struct i2c_driver g_dummy_driver = {
	.name = "dummy",
	.probe = dummy_probe,
	.id_table = g_dummy_id,
};

struct i2c_client *i2c_new_dummy_device(struct i2c_adapter *adap,
					uint16_t address)
{
	struct i2c_board_info info;

	kmemset(&info, 0, sizeof(info));
	i2c_strlcpy(info.type, "dummy", sizeof(info.type));
	info.addr = address;
	return i2c_new_client_device(adap, &info);
}

void i2c_unregister_device(struct i2c_client *client)
{
	if (I2C_IS_ERR_OR_NULL(client))
		return;
	if (WARN_ON(client->magic != I2C_CLIENT_MAGIC))
		return;
	mutex_lock(&g_i2c_core_lock);
	if (client->dead) {
		mutex_unlock(&g_i2c_core_lock);
		return;
	}
	client->dead = 1;
	list_del_init(&client->node);
	mutex_unlock(&g_i2c_core_lock);

	i2c_unbind(client);
	if (client->acpi_enumerated)
		i2c_acpi_forget_enumerated(client);
	/* The adapter list's reference. */
	i2c_client_put(client);
}

/* ==========================================================================
 * Drivers
 * ========================================================================== */

int i2c_register_driver(struct i2c_driver *drv)
{
	int slot = -1;

	might_sleep();
	if (WARN_ON(!drv || !drv->name))
		return -EINVAL;
	if (WARN_ON(!g_core_ready))
		return -EAGAIN;

	mutex_lock(&g_i2c_core_lock);
	if (drv->registered) {
		mutex_unlock(&g_i2c_core_lock);
		return -EBUSY;
	}
	for (int i = 0; i < I2C_MAX_DRIVERS; i++)
		if (!g_drivers[i]) {
			slot = i;
			break;
		}
	if (slot < 0) {
		mutex_unlock(&g_i2c_core_lock);
		kprintf("i2c: no room for driver %s\n", drv->name);
		return -ENOSPC;
	}
	g_drivers[slot] = drv;
	drv->slot = slot;
	drv->binding = 0;
	drv->registered = 1;
	mutex_unlock(&g_i2c_core_lock);

	kprintf("i2c: driver [%s] registered\n", drv->name);
	i2c_bind_all(0);
	return 0;
}

struct del_driver_ctx {
	struct i2c_driver *drv;
};

static int want_bound_to(const struct i2c_client *c, void *arg)
{
	return c->driver == ((struct del_driver_ctx *)arg)->drv;
}

/* Read inside a wait condition: no lock may be taken there. */
static int driver_idle(struct i2c_driver *drv)
{
	return __atomic_load_n(&drv->binding, __ATOMIC_ACQUIRE) == 0;
}

void i2c_del_driver(struct i2c_driver *drv)
{
	struct del_driver_ctx ctx = { drv };
	struct i2c_adapter **adaps;
	int na;

	might_sleep();
	if (!drv)
		return;
	mutex_lock(&g_i2c_core_lock);
	if (!drv->registered || drv->slot < 0 ||
	    drv->slot >= I2C_MAX_DRIVERS || g_drivers[drv->slot] != drv) {
		mutex_unlock(&g_i2c_core_lock);
		return;
	}
	g_drivers[drv->slot] = NULL;
	drv->registered = 0;
	mutex_unlock(&g_i2c_core_lock);

	/* No new bind picks the driver now; let the ones in flight end. */
	wait_event(g_bind_wq, driver_idle(drv));

	adaps = i2c_collect_adapters(&na);
	for (int a = 0; a < na; a++) {
		struct i2c_client **cl;
		int nc;

		cl = i2c_collect_clients(adaps[a], want_bound_to, &ctx, &nc);
		for (int i = 0; i < nc; i++) {
			mutex_lock(&cl[i]->bind_lock);
			if (cl[i]->driver == drv)
				i2c_device_remove(cl[i]);
			mutex_unlock(&cl[i]->bind_lock);
		}
		if (cl)
			i2c_put_clients(cl, nc);
	}
	if (adaps)
		i2c_put_adapters(adaps, na);
	drv->slot = -1;
	kprintf("i2c: driver [%s] unregistered\n", drv->name);
}

/* ==========================================================================
 * Adapters
 * ========================================================================== */

static int i2c_register_adapter(struct i2c_adapter *adap, int nr)
{
	int res;

	might_sleep();
	if (WARN_ON(!g_core_ready))
		return -EAGAIN;
	if (WARN(!adap->name[0], "i2c adapter has no name"))
		return -EINVAL;
	if (!adap->algo) {
		kprintf("i2c: adapter '%s': no algo supplied!\n", adap->name);
		return -EINVAL;
	}

	mutex_init(&adap->bus_lock, "i2c_bus_lock");
	adap->bus_lock_owner = NULL;
	adap->locked_flags = 0;
	refcount_set(&adap->ref, 1);
	wq_head_init(&adap->ref_wq, "i2c_adapter_ref");
	INIT_LIST_HEAD(&adap->clients);
	kmemset(&adap->stats, 0, sizeof(adap->stats));
	adap->dying = 0;
	adap->pm_suspended = 0;
	adap->i2c_dev = NULL;
	adap->acpi_gsb = NULL;
	/* One second unless the driver says otherwise. */
	if (adap->timeout <= 0)
		adap->timeout = 1000;
	if (adap->retries < 0)
		adap->retries = 0;

	mutex_lock(&g_i2c_core_lock);
	if (nr >= 0) {
		if (nr >= I2C_MAX_ADAPTERS || g_adapters[nr]) {
			mutex_unlock(&g_i2c_core_lock);
			kprintf("i2c: adapter '%s': bus number %d is not free\n",
				adap->name, nr);
			return -EBUSY;
		}
	} else {
		for (int i = 0; i < I2C_MAX_ADAPTERS; i++)
			if (!g_adapters[i]) {
				nr = i;
				break;
			}
		if (nr < 0) {
			mutex_unlock(&g_i2c_core_lock);
			kprintf("i2c: adapter '%s': no bus number left\n",
				adap->name);
			return -ENOSPC;
		}
	}
	adap->nr = nr;
	ksnprintf(adap->dev_name, sizeof(adap->dev_name), "i2c-%d", nr);
	g_adapters[nr] = adap;
	adap->registered = 1;
	mutex_unlock(&g_i2c_core_lock);

	res = i2c_init_recovery(adap);
	if (res == -EPROBE_DEFER) {
		mutex_lock(&g_i2c_core_lock);
		g_adapters[nr] = NULL;
		adap->registered = 0;
		mutex_unlock(&g_i2c_core_lock);
		return res;
	}

	kprintf("i2c %s: adapter [%s] registered%s%s\n", adap->dev_name,
		adap->name, adap->parent_name[0] ? " on " : "",
		adap->parent_name);

	res = i2c_dev_attach_adapter(adap);
	if (res)
		kprintf("i2c %s: no /dev node (%d)\n", adap->dev_name, res);
	if (adap->acpi_handle) {
		res = i2c_acpi_install_space_handler(adap);
		if (res && res != -ENODEV)
			kprintf("i2c %s: no GenericSerialBus region handler (%d)\n",
				adap->dev_name, res);
		i2c_acpi_register_devices(adap);
	}
	i2c_retry_deferred_probes();
	return 0;
}

int i2c_add_adapter(struct i2c_adapter *adap)
{
	if (WARN_ON(!adap))
		return -EINVAL;
	return i2c_register_adapter(adap, -1);
}

int i2c_add_numbered_adapter(struct i2c_adapter *adap)
{
	if (WARN_ON(!adap))
		return -EINVAL;
	if (adap->nr == -1)
		return i2c_add_adapter(adap);
	return i2c_register_adapter(adap, adap->nr);
}

static int want_non_dummy(const struct i2c_client *c, void *arg)
{
	(void)arg;
	return kstrcmp(c->name, "dummy") != 0;
}

static int want_any(const struct i2c_client *c, void *arg)
{
	(void)c;
	(void)arg;
	return 1;
}

static int adapter_unused(struct i2c_adapter *adap)
{
	return refcount_read(&adap->ref) == 1;
}

void i2c_del_adapter(struct i2c_adapter *adap)
{
	struct i2c_client **cl;
	int nc;

	might_sleep();
	if (!adap)
		return;
	mutex_lock(&g_i2c_core_lock);
	if (!adap->registered || adap->dying || adap->nr < 0 ||
	    adap->nr >= I2C_MAX_ADAPTERS || g_adapters[adap->nr] != adap) {
		mutex_unlock(&g_i2c_core_lock);
		kprintf("i2c: attempting to delete unregistered adapter [%s]\n",
			adap->name);
		return;
	}
	adap->dying = 1;
	mutex_unlock(&g_i2c_core_lock);

	i2c_acpi_remove_space_handler(adap);
	i2c_dev_detach_adapter(adap);

	/* Real devices first: their drivers may still want to clean up the
	 * dummy clients they created; then whatever is left. */
	cl = i2c_collect_clients(adap, want_non_dummy, NULL, &nc);
	for (int i = 0; i < nc; i++)
		i2c_unregister_device(cl[i]);
	if (cl)
		i2c_put_clients(cl, nc);
	cl = i2c_collect_clients(adap, want_any, NULL, &nc);
	for (int i = 0; i < nc; i++)
		i2c_unregister_device(cl[i]);
	if (cl)
		i2c_put_clients(cl, nc);

	/* Every i2c_get_adapter() user must be gone (open /dev/i2c-N files
	 * close) before the driver may free the adapter. */
	wait_event(adap->ref_wq, adapter_unused(adap));

	i2c_dev_free_adapter(adap);
	mutex_lock(&g_i2c_core_lock);
	WARN_ON(!list_empty(&adap->clients));
	g_adapters[adap->nr] = NULL;
	adap->registered = 0;
	mutex_unlock(&g_i2c_core_lock);
	kprintf("i2c %s: adapter [%s] unregistered\n", adap->dev_name,
		adap->name);
}

struct i2c_adapter *i2c_get_adapter(int nr)
{
	struct i2c_adapter *adap;

	if (nr < 0 || nr >= I2C_MAX_ADAPTERS)
		return NULL;
	mutex_lock(&g_i2c_core_lock);
	adap = g_adapters[nr];
	if (adap && adap->registered && !adap->dying)
		refcount_inc(&adap->ref);
	else
		adap = NULL;
	mutex_unlock(&g_i2c_core_lock);
	return adap;
}

void i2c_put_adapter(struct i2c_adapter *adap)
{
	if (!adap)
		return;
	/* The registration's own reference keeps the count above zero. */
	if (WARN_ON(refcount_dec_and_test(&adap->ref)))
		return;
	wake_up(&adap->ref_wq);
}

struct i2c_adapter *i2c_find_adapter_by_acpi_handle(void *handle)
{
	struct i2c_adapter *found = NULL;

	if (!handle)
		return NULL;
	mutex_lock(&g_i2c_core_lock);
	for (int nr = 0; nr < I2C_MAX_ADAPTERS; nr++) {
		struct i2c_adapter *a = g_adapters[nr];

		if (a && a->registered && !a->dying &&
		    a->acpi_handle == handle) {
			refcount_inc(&a->ref);
			found = a;
			break;
		}
	}
	mutex_unlock(&g_i2c_core_lock);
	return found;
}

/* ==========================================================================
 * Power management
 * ========================================================================== */

void i2c_mark_adapter_suspended(struct i2c_adapter *adap)
{
	i2c_lock_bus(adap, I2C_LOCK_ROOT_ADAPTER);
	set_flag(&adap->locked_flags, I2C_ALF_IS_SUSPENDED);
	i2c_unlock_bus(adap, I2C_LOCK_ROOT_ADAPTER);
}

void i2c_mark_adapter_resumed(struct i2c_adapter *adap)
{
	i2c_lock_bus(adap, I2C_LOCK_ROOT_ADAPTER);
	clear_flag(&adap->locked_flags, I2C_ALF_IS_SUSPENDED);
	clear_flag(&adap->locked_flags, I2C_ALF_SUSPEND_REPORTED);
	i2c_unlock_bus(adap, I2C_LOCK_ROOT_ADAPTER);
}

static int want_bound(const struct i2c_client *c, void *arg)
{
	(void)arg;
	return c->driver != NULL;
}

static int i2c_client_suspend(struct i2c_client *client)
{
	int ret = 0;

	mutex_lock(&client->bind_lock);
	if (client->driver && !client->suspended) {
		if (client->driver->suspend)
			ret = client->driver->suspend(client);
		if (!ret) {
			client->suspended = 1;
			if ((client->flags & I2C_CLIENT_WAKE) &&
			    client->irq > 0 && !enable_irq_wake(client->irq))
				client->wake_armed = 1;
		} else {
			kprintf("i2c %s: suspend failed (%d)\n",
				client->dev_name, ret);
		}
	}
	mutex_unlock(&client->bind_lock);
	return ret;
}

static void i2c_client_resume(struct i2c_client *client)
{
	mutex_lock(&client->bind_lock);
	if (client->driver && client->suspended) {
		int ret = 0;

		if (client->wake_armed && client->irq > 0)
			disable_irq_wake(client->irq);
		client->wake_armed = 0;
		if (client->driver->resume)
			ret = client->driver->resume(client);
		if (ret)
			kprintf("i2c %s: resume failed (%d)\n",
				client->dev_name, ret);
		client->suspended = 0;
	}
	mutex_unlock(&client->bind_lock);
}

int i2c_pm_resume_all(void)
{
	struct i2c_adapter **adaps;
	int na;

	might_sleep();
	adaps = i2c_collect_adapters(&na);
	/* Adapters first, then their clients. */
	for (int a = 0; a < na; a++) {
		struct i2c_adapter *adap = adaps[a];

		if (adap->pm_suspended) {
			int ret = adap->pm && adap->pm->resume ?
					  adap->pm->resume(adap) :
					  0;

			if (ret)
				kprintf("i2c %s: resume failed (%d)\n",
					adap->dev_name, ret);
			adap->pm_suspended = 0;
		}
	}
	for (int a = 0; a < na; a++) {
		struct i2c_client **cl;
		int nc;

		cl = i2c_collect_clients(adaps[a], want_bound, NULL, &nc);
		for (int i = 0; i < nc; i++)
			i2c_client_resume(cl[i]);
		if (cl)
			i2c_put_clients(cl, nc);
	}
	if (adaps)
		i2c_put_adapters(adaps, na);
	return 0;
}

int i2c_pm_suspend_all(void)
{
	struct i2c_adapter **adaps;
	int na, ret = 0;

	might_sleep();
	adaps = i2c_collect_adapters(&na);
	/* Clients first (last bus, last client first), then adapters. */
	for (int a = na - 1; a >= 0 && !ret; a--) {
		struct i2c_client **cl;
		int nc;

		cl = i2c_collect_clients(adaps[a], want_bound, NULL, &nc);
		for (int i = nc - 1; i >= 0 && !ret; i--)
			ret = i2c_client_suspend(cl[i]);
		if (cl)
			i2c_put_clients(cl, nc);
	}
	for (int a = na - 1; a >= 0 && !ret; a--) {
		struct i2c_adapter *adap = adaps[a];

		if (adap->pm && adap->pm->suspend) {
			ret = adap->pm->suspend(adap);
			if (ret) {
				kprintf("i2c %s: suspend failed (%d)\n",
					adap->dev_name, ret);
				break;
			}
		}
		adap->pm_suspended = 1;
	}
	if (adaps)
		i2c_put_adapters(adaps, na);
	if (ret)
		(void)i2c_pm_resume_all(); /* undo what was suspended */
	return ret;
}

/* The suspend test (one at a time) and what it found. */
static DEFINE_MUTEX(g_pm_test_lock);
static struct {
	unsigned long runs, failures;
	int last_err; /* of the last run */
	unsigned int last_ms;
	uint64_t suspend_us, resume_us; /* of the last run */
} g_pm_test;

int i2c_pm_suspend_test(unsigned int ms)
{
	uint64_t t0, t1, t2, t3;
	int ret;

	might_sleep();
	if (!mutex_trylock(&g_pm_test_lock))
		return -EBUSY;
	kprintf("i2c: suspend test: suspending I2C clients, adapters and GPIO controllers for %u ms\n",
		ms);
	t0 = hrtimer_now_ns();
	/* Devices first: clients, then their adapters ... */
	ret = i2c_pm_suspend_all();
	if (ret) {
		/* i2c_pm_suspend_all resumed what it had suspended. */
		kprintf("i2c: suspend test: suspend failed (%d), resumed\n",
			ret);
		goto out;
	}
	/* ... then the GPIO controllers their interrupts come through. */
	gpio_pinctrl_suspend_all();
	t1 = hrtimer_now_ns();
	if (ms)
		msleep(ms);
	t2 = hrtimer_now_ns();
	/* Resume in the reverse order. */
	gpio_pinctrl_resume_all();
	ret = i2c_pm_resume_all();
	t3 = hrtimer_now_ns();
	g_pm_test.suspend_us = (t1 - t0) / 1000;
	g_pm_test.resume_us = (t3 - t2) / 1000;
	kprintf("i2c: suspend test: done (%d), suspend %lu us, resume %lu us\n",
		ret, (unsigned long)g_pm_test.suspend_us,
		(unsigned long)g_pm_test.resume_us);
out:
	g_pm_test.runs++;
	if (ret)
		g_pm_test.failures++;
	g_pm_test.last_err = ret;
	g_pm_test.last_ms = ms;
	mutex_unlock(&g_pm_test_lock);
	return ret;
}

static void i2c_client_shutdown(struct i2c_client *client)
{
	mutex_lock(&client->bind_lock);
	if (client->driver) {
		if (client->driver->shutdown)
			client->driver->shutdown(client);
		else if (client->irq > 0)
			disable_irq(client->irq);
	}
	mutex_unlock(&client->bind_lock);
}

void i2c_shutdown_adapter_clients(struct i2c_adapter *adap)
{
	struct i2c_client **cl;
	int nc;

	cl = i2c_collect_clients(adap, want_bound, NULL, &nc);
	for (int i = nc - 1; i >= 0; i--)
		i2c_client_shutdown(cl[i]);
	if (cl)
		i2c_put_clients(cl, nc);
}

void i2c_shutdown_all(void)
{
	struct i2c_adapter **adaps;
	int na;

	adaps = i2c_collect_adapters(&na);
	for (int a = na - 1; a >= 0; a--) {
		i2c_shutdown_adapter_clients(adaps[a]);
		if (adaps[a]->pm && adaps[a]->pm->shutdown)
			adaps[a]->pm->shutdown(adaps[a]);
	}
	if (adaps)
		i2c_put_adapters(adaps, na);
}

/* ==========================================================================
 * /proc
 * ========================================================================== */

/* One client's line (and its ACPI line). */
static long i2c_client_show_line(struct i2c_client *c, char *buf, long cap,
				 long p)
{
	p = i2c_show_printf(buf, cap, p,
			    "  client 0x%02x %s [%s] irq %d%s driver %s%s%s\n",
			    c->addr, c->name, c->dev_name, c->irq,
			    (c->flags & I2C_CLIENT_WAKE) ? " wake" : "",
			    c->driver ? c->driver->name : "-",
			    c->deferred ? " (deferred)" : "",
			    c->suspended ? " (suspended)" : "");
	if (c->acpi_dev)
		p = i2c_show_printf(buf, cap, p, "    acpi %s hid %s\n",
				    c->acpi_dev->path, c->acpi_dev->hid);
	return p;
}

/* The registry lock is not held while an adapter's or a driver's show
 * callback runs (they may sleep, take their own locks, or call back into
 * the core): adapters and clients are walked through references. */
long i2c_core_show(char *buf, long cap)
{
	struct i2c_adapter **adaps;
	long p = 0;
	int na;

	if (g_pm_test.runs)
		p = i2c_show_printf(buf, cap, p,
				    "suspend-test: runs %lu failures %lu last %d (%u ms asleep, suspend %lu us, resume %lu us)\n",
				    g_pm_test.runs, g_pm_test.failures,
				    g_pm_test.last_err, g_pm_test.last_ms,
				    (unsigned long)g_pm_test.suspend_us,
				    (unsigned long)g_pm_test.resume_us);
	adaps = i2c_collect_adapters(&na);
	for (int i = 0; i < na; i++) {
		struct i2c_adapter *a = adaps[i];
		const struct i2c_adapter_stats *s = &a->stats;
		struct i2c_client **cl;
		int nc;

		p = i2c_show_printf(buf, cap, p, "%s: %s [%s] %u kHz%s%s\n",
				    a->dev_name, a->name,
				    a->parent_name[0] ? a->parent_name : "-",
				    a->bus_freq_hz / 1000,
				    i2c_adapter_is_suspended(a) ?
					    " suspended" :
					    "",
				    a->dying ? " removing" : "");
		p = i2c_show_printf(buf, cap, p,
				    "  transfers %lu msgs %lu bytes %lu ok %lu nack %lu arblost %lu timeout %lu other %lu\n",
				    (unsigned long)s->xfers,
				    (unsigned long)s->msgs,
				    (unsigned long)s->bytes,
				    (unsigned long)s->ok,
				    (unsigned long)s->err_nack,
				    (unsigned long)s->err_arb,
				    (unsigned long)s->err_timeout,
				    (unsigned long)s->err_other);
		p = i2c_show_printf(buf, cap, p,
				    "  retries %lu recoveries %lu (failed %lu) quirk-rejects %lu suspended-rejects %lu smbus %lu timeout %d ms retries %d recovery %s\n",
				    (unsigned long)s->retries,
				    (unsigned long)s->recoveries,
				    (unsigned long)s->recovery_failed,
				    (unsigned long)s->rejected_quirk,
				    (unsigned long)s->rejected_suspended,
				    (unsigned long)s->smbus, a->timeout,
				    a->retries,
				    a->bus_recovery_info ? "yes" : "no");
		if (a->show)
			p = a->show(a, buf, cap, p);
		cl = i2c_collect_clients(a, want_any, NULL, &nc);
		for (int j = 0; j < nc; j++) {
			struct i2c_client *c = cl[j];

			/* The driver's counters under the bind lock (no
			 * unbind meanwhile); only tried: a client in the
			 * middle of a probe or unbind is skipped rather than
			 * waited for. */
			if (mutex_trylock(&c->bind_lock)) {
				p = i2c_client_show_line(c, buf, cap, p);
				if (c->driver && c->driver->show)
					p = c->driver->show(c, buf, cap, p);
				mutex_unlock(&c->bind_lock);
			} else {
				p = i2c_client_show_line(c, buf, cap, p);
			}
		}
		if (cl)
			i2c_put_clients(cl, nc);
	}
	if (adaps)
		i2c_put_adapters(adaps, na);
	if (!na)
		p = i2c_show_printf(buf, cap, p, "no I2C adapters\n");
	return p;
}

long i2c_proc_show(struct pfs_node *n, char *buf, long cap)
{
	(void)n;
	return i2c_core_show(buf, cap);
}

/* "suspend-test [ms]": the system sleep chain (default 1000 ms asleep,
 * at most 60 s). */
long i2c_proc_store(struct pfs_node *n, const char *buf, long len)
{
	static const char cmd[] = "suspend-test";
	const long clen = (long)sizeof(cmd) - 1;
	unsigned long ms = 1000;
	const long written = len;
	long i;
	int ret;

	(void)n;
	if (!capable())
		return -EPERM;
	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == ' ' ||
			   buf[len - 1] == '\t'))
		len--;
	if (len < clen || kmemcmp(buf, cmd, (size_t)clen))
		return -EINVAL;
	i = clen;
	if (i < len) {
		if (buf[i] != ' ' && buf[i] != '\t')
			return -EINVAL;
		while (i < len && (buf[i] == ' ' || buf[i] == '\t'))
			i++;
		if (i >= len)
			return -EINVAL;
		ms = 0;
		for (; i < len; i++) {
			if (buf[i] < '0' || buf[i] > '9')
				return -EINVAL;
			ms = ms * 10 + (unsigned long)(buf[i] - '0');
			if (ms > 60000)
				return -EINVAL;
		}
	}
	ret = i2c_pm_suspend_test((unsigned int)ms);
	/* The write itself succeeded; the result is in /proc/i2c and the
	 * log.  -EBUSY (a test already running) is the writer's error. */
	return ret == -EBUSY ? -EBUSY : written;
}

long i2c_proc_bus_show(struct pfs_node *n, char *buf, long cap)
{
	long p = 0;

	(void)n;
	mutex_lock(&g_i2c_core_lock);
	for (int nr = 0; nr < I2C_MAX_ADAPTERS; nr++) {
		struct i2c_adapter *a = g_adapters[nr];

		if (a && a->registered && !a->dying)
			p = i2c_show_printf(buf, cap, p,
					    "%s\ti2c\t%s\tI2C adapter\n",
					    a->dev_name, a->name);
	}
	mutex_unlock(&g_i2c_core_lock);
	return p;
}

/* ==========================================================================
 * Initialisation
 * ========================================================================== */

void i2c_core_init(void)
{
	int rc;

	if (g_core_ready)
		return;
	g_core_ready = 1;
	rc = i2c_dev_init();
	if (rc)
		kprintf("i2c: /dev entries unavailable (%d)\n", rc);
	rc = i2c_register_driver(&g_dummy_driver);
	if (rc)
		kprintf("i2c: dummy driver not registered (%d)\n", rc);
}

void i2c_core_late_init(void)
{
	int adapters = 0, clients = 0, bound = 0, deferred = 0;

	i2c_retry_deferred_probes();
	mutex_lock(&g_i2c_core_lock);
	for (int nr = 0; nr < I2C_MAX_ADAPTERS; nr++) {
		struct i2c_adapter *a = g_adapters[nr];
		struct i2c_client *c;

		if (!a || !a->registered)
			continue;
		adapters++;
		list_for_each_entry(c, &a->clients, node) {
			clients++;
			if (c->driver)
				bound++;
			else if (c->deferred) {
				deferred++;
				kprintf("i2c %s: probe still deferred (%d)\n",
					c->dev_name, c->probe_err);
			}
		}
	}
	mutex_unlock(&g_i2c_core_lock);
	kprintf("i2c: %d adapter%s, %d client%s (%d bound, %d deferred)\n",
		adapters, adapters == 1 ? "" : "s", clients,
		clients == 1 ? "" : "s", bound, deferred);
}
