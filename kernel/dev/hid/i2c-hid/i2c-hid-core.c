// LikeOS I2C-HID - HID over I2C protocol
//
// See i2c-hid.h for the device's registers.  This file:
//
//   probe     check that something answers at the address, fetch and check
//             the 30-byte HID descriptor, request the interrupt (one-shot,
//             threaded, level-low when firmware gives no trigger), register
//             a HID device.  The HID core then calls parse(), which powers
//             the device on, resets it (waiting for the zero-length "reset
//             done" input report), and reads the report descriptor.
//   input     the threaded interrupt handler reads wMaxInputLength bytes
//             from the device: a length of 0 acknowledges a reset, a length
//             outside 2..read size is garbage, the rest is a report for the
//             HID core (while an input node of the device is open).
//   requests  GET_REPORT / SET_REPORT through the command and data
//             registers, output reports through the output register.
//   power     SET_POWER SLEEP on suspend, ON (or a reset) on resume.
//   recovery  a device that keeps failing its input reads, or whose reads
//             time out, has its interrupt masked and is reset from a work
//             item, after which the HID driver re-sends its configuration
//             (input mode); a device that reset on its own gets its
//             configuration re-sent.  Both are rate limited; a device that
//             keeps failing is left disabled until the next resume.
//
// Every number the device sends is treated as hostile: descriptor fields
// are checked before use, every length is clamped to the buffer it is used
// with, and buffers are sized from the parsed report descriptor (the
// largest report of any kind), never from what the device claims.
//
// Locking:
//   reset_lock   a reset in progress (no feature requests may interleave:
//                the device would drop its reset acknowledgement); taken
//                around every request too
//   cmd_lock     cmdbuf / rawbuf and the buffer size
//   flags        atomic bit operations
// Order: reset_lock -> cmd_lock -> (I2C bus lock).  The interrupt thread
// takes neither lock: it uses inbuf, which is only replaced with the
// interrupt disabled.  hid_input_report() never sleeps on the HID core's
// locks (trylock), so the thread never waits for a probe or remove of the
// HID driver.
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) 2012 Benjamin Tissoires, 2012 Ecole Nationale de l'Aviation Civile, 2012 Red Hat, Inc

#include <kernel/dev/hid/i2c-hid/i2c-hid.h>
#include <kernel/dev/hid/core/hid.h>
#include <kernel/dev/i2c/i2c.h>
#include <kernel/ke/irq.h>
#include <kernel/ke/delay.h>
#include <kernel/ke/mutex.h>
#include <kernel/ke/waitq.h>
#include <kernel/ke/workqueue.h>
#include <kernel/ke/timer.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>
#include <kernel/ke/syscall.h> /* errno values */

/* Command opcodes */
#define I2C_HID_OPCODE_RESET 0x01
#define I2C_HID_OPCODE_GET_REPORT 0x02
#define I2C_HID_OPCODE_SET_REPORT 0x03
#define I2C_HID_OPCODE_GET_IDLE 0x04
#define I2C_HID_OPCODE_SET_IDLE 0x05
#define I2C_HID_OPCODE_GET_PROTOCOL 0x06
#define I2C_HID_OPCODE_SET_PROTOCOL 0x07
#define I2C_HID_OPCODE_SET_POWER 0x08

/* Report types in the command's type field. */
#define I2C_HID_TYPE_INPUT 0x01
#define I2C_HID_TYPE_OUTPUT 0x02
#define I2C_HID_TYPE_FEATURE 0x03

/* ihid->flags bits */
#define I2C_HID_STARTED 0 // an input node is open: deliver reports
#define I2C_HID_RESET_PENDING 1 // a reset was sent, its ack not seen
#define I2C_HID_HW_STARTED 2 // between ll start() and stop()
#define I2C_HID_SUSPENDED 3 // suspend ran, resume not yet
#define I2C_HID_REMOVING 4 // remove/shutdown started
#define I2C_HID_ADDED 5 // the HID device was added
#define I2C_HID_RECOVER_RESET 6 // the work must reset the device
#define I2C_HID_RECOVER_REINIT 7 // the work must re-send the config
#define I2C_HID_IRQ_OFF_RECOVERY 8 // the thread masked the interrupt
#define I2C_HID_DEAD 9 // gave up after too many failures
#define I2C_HID_RECOVERY_BUSY 10 // a recovery is queued or running

#define I2C_HID_PWR_ON 0x00
#define I2C_HID_PWR_SLEEP 0x01

/* Smallest buffer before the report descriptor is known. */
#define I2C_HID_MIN_BUFFER_SIZE 64
/* How long a reset may take to be acknowledged. */
#define I2C_HID_RESET_TIMEOUT_MS 1000
/* Consecutive failed input reads before the device is reset; a read that
 * timed out (the controller was reset and the bus recovered) counts as
 * this many on its own. */
#define I2C_HID_RECOVERY_ERRORS 10
#define I2C_HID_TIMEOUT_WEIGHT 5
/* At most this many recoveries (resets and re-inits) per window. */
#define I2C_HID_RECOVERY_MAX 3
#define I2C_HID_RECOVERY_WINDOW_NS (60ull * 1000000000ull)
/* Error messages of the interrupt path: a burst per interval. */
#define I2C_HID_RL_BURST 10
#define I2C_HID_RL_INTERVAL_NS (5ull * 1000000000ull)

struct i2c_hid {
	struct i2c_client *client;
	struct hid_device *hid;
	struct i2c_hid_desc hdesc; // as received (little-endian)
	uint16_t wHIDDescRegister;
	unsigned int bufsize; // inbuf/rawbuf bytes (cmd_lock)
	uint8_t *inbuf; // the interrupt thread's
	uint8_t *rawbuf; // GET_REPORT answers (cmd_lock)
	uint8_t *cmdbuf; // commands and outgoing reports (cmd_lock)

	unsigned long flags; // I2C_HID_* bits (atomic)
	unsigned long quirks; // I2C_HID_QUIRK_*

	struct wait_queue_head wait; // reset acknowledgement
	struct mutex cmd_lock;
	struct mutex reset_lock;

	struct i2chid_ops *ops;
	int irq;
	int irq_requested;

	/* Recovery: the interrupt thread counts, the work acts. */
	struct work_struct recovery_work;
	unsigned int io_errors; // consecutive (interrupt thread only)
	struct mutex recovery_lock; // the window below
	uint64_t recovery_window_start;
	unsigned int recovery_count;
	int recovery_warned;

	/* Message rate limit of the interrupt path. */
	uint64_t rl_start;
	unsigned int rl_count, rl_missed;

	struct i2c_hid_stats stats;
};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static inline void ihid_set(struct i2c_hid *ihid, int bit)
{
	__atomic_fetch_or(&ihid->flags, 1ul << bit, __ATOMIC_SEQ_CST);
}

static inline void ihid_clear(struct i2c_hid *ihid, int bit)
{
	__atomic_fetch_and(&ihid->flags, ~(1ul << bit), __ATOMIC_SEQ_CST);
}

static inline int ihid_test(const struct i2c_hid *ihid, int bit)
{
	return (__atomic_load_n(&ihid->flags, __ATOMIC_SEQ_CST) >> bit) & 1;
}

static inline int ihid_test_and_clear(struct i2c_hid *ihid, int bit)
{
	return (__atomic_fetch_and(&ihid->flags, ~(1ul << bit),
				   __ATOMIC_SEQ_CST) >>
		bit) &
	       1;
}

static inline int ihid_test_and_set(struct i2c_hid *ihid, int bit)
{
	return (__atomic_fetch_or(&ihid->flags, 1ul << bit,
				  __ATOMIC_SEQ_CST) >>
		bit) &
	       1;
}

static inline uint16_t le16(const void *p)
{
	const uint8_t *b = p;

	return (uint16_t)(b[0] | (b[1] << 8));
}

static inline void put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static inline void ihid_stat_inc(unsigned long *c)
{
	__atomic_fetch_add(c, 1, __ATOMIC_RELAXED);
}

static const char *ihid_drv_name(const struct i2c_hid *ihid)
{
	const struct i2c_client *c = ihid->client;

	return c->driver && c->driver->name ? c->driver->name : "i2c_hid";
}

#define ihid_err(ihid, fmt, ...)                                          \
	kprintf("%s %s: " fmt, ihid_drv_name(ihid), (ihid)->client->dev_name, \
		##__VA_ARGS__)
#define ihid_warn(ihid, fmt, ...) ihid_err(ihid, fmt, ##__VA_ARGS__)
#define ihid_info(ihid, fmt, ...) ihid_err(ihid, fmt, ##__VA_ARGS__)

/* Message budget of the interrupt path (called from the thread only). */
static int ihid_ratelimit(struct i2c_hid *ihid)
{
	uint64_t now = timer_get_precise_ns();

	if (!ihid->rl_start || now - ihid->rl_start > I2C_HID_RL_INTERVAL_NS) {
		if (ihid->rl_missed)
			ihid_warn(ihid, "%u input error messages suppressed\n",
				  ihid->rl_missed);
		ihid->rl_start = now ? now : 1;
		ihid->rl_count = 0;
		ihid->rl_missed = 0;
	}
	if (ihid->rl_count < I2C_HID_RL_BURST) {
		ihid->rl_count++;
		return 1;
	}
	ihid->rl_missed++;
	return 0;
}

// ---------------------------------------------------------------------------
// Device quirks
// ---------------------------------------------------------------------------

static const struct i2c_hid_quirks {
	uint16_t idVendor;
	uint16_t idProduct;
	uint32_t quirks;
} i2c_hid_quirks[] = {
	{ I2C_VENDOR_ID_HANTICK, I2C_PRODUCT_ID_HANTICK_5288,
	  I2C_HID_QUIRK_NO_IRQ_AFTER_RESET | I2C_HID_QUIRK_BAD_INPUT_SIZE },
	{ I2C_VENDOR_ID_ITE, I2C_DEVICE_ID_ITE_VOYO_WINPAD_A15,
	  I2C_HID_QUIRK_NO_IRQ_AFTER_RESET },
	{ USB_VENDOR_ID_ITE, I2C_DEVICE_ID_ITE_LENOVO_YOGA_SLIM_7X_G11_KEYBOARD,
	  I2C_HID_QUIRK_NO_IRQ_AFTER_RESET },
	{ I2C_VENDOR_ID_RAYDIUM, I2C_PRODUCT_ID_RAYDIUM_3118,
	  I2C_HID_QUIRK_NO_IRQ_AFTER_RESET },
	{ USB_VENDOR_ID_ALPS_JP, (uint16_t)HID_ANY_ID,
	  I2C_HID_QUIRK_RESET_ON_RESUME },
	{ I2C_VENDOR_ID_SYNAPTICS, I2C_PRODUCT_ID_SYNAPTICS_SYNA2393,
	  I2C_HID_QUIRK_RESET_ON_RESUME },
	{ USB_VENDOR_ID_ITE, I2C_DEVICE_ID_ITE_LENOVO_LEGION_Y720,
	  I2C_HID_QUIRK_BAD_INPUT_SIZE },
	{ I2C_VENDOR_ID_CIRQUE, I2C_PRODUCT_ID_CIRQUE_1063,
	  I2C_HID_QUIRK_NO_SLEEP_ON_SUSPEND },
	{ I2C_VENDOR_ID_CIRQUE, I2C_PRODUCT_ID_CIRQUE_D0C1,
	  I2C_HID_QUIRK_NO_IRQ_AFTER_RESET },
	/* Without an additional power on command, at least some QTEC
	 * devices send garbage. */
	{ I2C_VENDOR_ID_QTEC, (uint16_t)HID_ANY_ID, I2C_HID_QUIRK_RE_POWER_ON },
	/* Sending the wakeup after reset actually breaks ELAN touchscreen
	 * controllers. */
	{ USB_VENDOR_ID_ELAN, (uint16_t)HID_ANY_ID,
	  I2C_HID_QUIRK_NO_WAKEUP_AFTER_RESET | I2C_HID_QUIRK_BOGUS_IRQ },
	{ I2C_VENDOR_ID_GOODIX, I2C_DEVICE_ID_GOODIX_0D42,
	  I2C_HID_QUIRK_DELAY_WAKEUP_AFTER_RESUME },
	{ I2C_VENDOR_ID_BLTP, I2C_PRODUCT_ID_BLTP7853,
	  I2C_HID_QUIRK_NO_IRQ_AFTER_RESET },
	{ 0, 0, 0 }
};

/* The last matching entry wins (the table lists vendor-wide entries
 * before specific ones where both exist). */
uint32_t i2c_hid_lookup_quirk(uint16_t idVendor, uint16_t idProduct)
{
	uint32_t quirks = 0;

	for (int n = 0; i2c_hid_quirks[n].idVendor; n++)
		if (i2c_hid_quirks[n].idVendor == idVendor &&
		    (i2c_hid_quirks[n].idProduct == (uint16_t)HID_ANY_ID ||
		     i2c_hid_quirks[n].idProduct == idProduct))
			quirks = i2c_hid_quirks[n].quirks;
	return quirks;
}

// ---------------------------------------------------------------------------
// Transfers
// ---------------------------------------------------------------------------

/* Some STM-based devices need 400 us after a rising clock edge to wake
 * from deep sleep, in which case the first read fails; certain Weida Tech
 * devices too.  Try again after a short sleep. */
static int i2c_hid_probe_address(struct i2c_hid *ihid)
{
	int ret;

	ret = i2c_smbus_read_byte(ihid->client);
	if (ret < 0) {
		usleep_range(400, 500);
		ret = i2c_smbus_read_byte(ihid->client);
	}
	return ret < 0 ? ret : 0;
}

/* Send `send_len' bytes, then (combined, with a repeated START) receive
 * `recv_len'.  0 or -errno. */
static int i2c_hid_xfer(struct i2c_hid *ihid, uint8_t *send_buf, int send_len,
			uint8_t *recv_buf, int recv_len)
{
	struct i2c_client *client = ihid->client;
	struct i2c_msg msgs[2];
	int n = 0;
	int ret;

	might_sleep();
	/* Lengths come from report sizes the device declared: refused, not
	 * asserted, when they do not fit the 16-bit message length. */
	if (WARN_RATELIMIT(send_len < 0 || recv_len < 0 || send_len > 0xffff ||
				   recv_len > 0xffff,
			   "i2c-hid: transfer of %d/%d bytes refused", send_len,
			   recv_len))
		return -EINVAL;
	kmemset(msgs, 0, sizeof(msgs));
	if (send_len) {
		msgs[n].addr = client->addr;
		msgs[n].flags =
			(uint16_t)((client->flags & I2C_M_TEN) | I2C_M_DMA_SAFE);
		msgs[n].len = (uint16_t)send_len;
		msgs[n].buf = send_buf;
		n++;
	}
	if (recv_len) {
		msgs[n].addr = client->addr;
		msgs[n].flags = (uint16_t)((client->flags & I2C_M_TEN) |
					   I2C_M_RD | I2C_M_DMA_SAFE);
		msgs[n].len = (uint16_t)recv_len;
		msgs[n].buf = recv_buf;
		n++;
	}
	if (WARN_ON_ONCE(!n))
		return -EINVAL;

	ret = i2c_transfer(client->adapter, msgs, n);
	if (ret != n)
		return ret < 0 ? ret : -EIO;
	return 0;
}

/* Read `len' bytes from a 16-bit register. */
static int i2c_hid_read_register(struct i2c_hid *ihid, uint16_t reg, void *buf,
				 size_t len)
{
	int ret;

	if (len > 0xffff)
		return -EINVAL;
	mutex_lock(&ihid->cmd_lock);
	put_le16(ihid->cmdbuf, reg);
	ret = i2c_hid_xfer(ihid, ihid->cmdbuf, 2, buf, (int)len);
	mutex_unlock(&ihid->cmd_lock);
	return ret;
}

/* The command's type/id byte(s) and opcode: report IDs of 15 and above
 * do not fit the 4-bit field, which then says 0xF and the ID follows the
 * opcode. */
static size_t i2c_hid_encode_command(uint8_t *buf, uint8_t opcode,
				     int report_type, int report_id)
{
	size_t length = 0;

	if (report_id < 0x0F) {
		buf[length++] = (uint8_t)(report_type << 4 | report_id);
		buf[length++] = opcode;
	} else {
		buf[length++] = (uint8_t)(report_type << 4 | 0x0F);
		buf[length++] = opcode;
		buf[length++] = (uint8_t)report_id;
	}
	return length;
}

/* GET_REPORT: the answer comes from the data register, length first.
 * Copies at most recv_len bytes of report data (its ID byte included for
 * numbered reports) to recv_buf.  Returns the bytes copied, 0 for an
 * empty answer, or -errno. */
static int i2c_hid_get_report(struct i2c_hid *ihid, uint8_t report_type,
			      uint8_t report_id, uint8_t *recv_buf,
			      size_t recv_len)
{
	size_t length = 0;
	size_t ret_count;
	int error;

	mutex_lock(&ihid->cmd_lock);
	if (WARN_ON_ONCE(ihid->bufsize < 2)) {
		mutex_unlock(&ihid->cmd_lock);
		return -EINVAL;
	}
	/* The command register goes first, then the command, then the data
	 * register the answer is read from (not aligned after a 3-byte
	 * command). */
	put_le16(ihid->cmdbuf, le16(&ihid->hdesc.wCommandRegister));
	length += 2;
	length += i2c_hid_encode_command(ihid->cmdbuf + length,
					 I2C_HID_OPCODE_GET_REPORT, report_type,
					 report_id);
	put_le16(ihid->cmdbuf + length, le16(&ihid->hdesc.wDataRegister));
	length += 2;

	/* The device puts the length in front of the data. */
	if (recv_len > ihid->bufsize - 2)
		recv_len = ihid->bufsize - 2;
	error = i2c_hid_xfer(ihid, ihid->cmdbuf, (int)length, ihid->rawbuf,
			     (int)(recv_len + 2));
	if (error) {
		mutex_unlock(&ihid->cmd_lock);
		ihid_err(ihid, "failed to get a report from device: %d\n",
			 error);
		return error;
	}

	ret_count = le16(ihid->rawbuf);
	/* An empty answer. */
	if (ret_count <= 2) {
		mutex_unlock(&ihid->cmd_lock);
		return 0;
	}
	if (recv_len > ret_count - 2)
		recv_len = ret_count - 2;
	kmemcpy(recv_buf, ihid->rawbuf + 2, recv_len);
	mutex_unlock(&ihid->cmd_lock);

	if (report_id && recv_len != 0 && recv_buf[0] != report_id) {
		ihid_err(ihid,
			 "device returned incorrect report (%d vs %d expected)\n",
			 recv_buf[0], report_id);
		return -EINVAL;
	}
	return (int)recv_len;
}

/* [length lo, hi, (report id,) data...]: the length counts itself. */
static size_t i2c_hid_format_report(uint8_t *buf, int report_id,
				    const uint8_t *data, size_t size)
{
	size_t length = 2;

	if (report_id)
		buf[length++] = (uint8_t)report_id;
	kmemcpy(buf + length, data, size);
	length += size;
	put_le16(buf, (uint16_t)length);
	return length;
}

/* Send a report: with SET_REPORT through the command and data registers
 * (do_set), or as a plain output report to the output register.  `buf'
 * is the data without the report ID.  Returns data_len or -errno. */
static int i2c_hid_set_or_send_report(struct i2c_hid *ihid,
				      uint8_t report_type, uint8_t report_id,
				      const uint8_t *buf, size_t data_len,
				      bool do_set)
{
	size_t length = 0;
	int error;

	if (!do_set && le16(&ihid->hdesc.wMaxOutputLength) == 0)
		return -ENOSYS;

	mutex_lock(&ihid->cmd_lock);
	/* cmdbuf holds a worst-case command plus bufsize bytes of data.  And
	 * the whole message -- registers, command, length, ID, data, at most
	 * 10 bytes more than the data -- and the report's own length field
	 * have to fit 16 bits. */
	if (data_len > ihid->bufsize ||
	    WARN_RATELIMIT(data_len > 0xffff - 10,
			   "i2c-hid: %lu-byte report refused",
			   (unsigned long)data_len)) {
		mutex_unlock(&ihid->cmd_lock);
		return -EINVAL;
	}
	if (do_set) {
		put_le16(ihid->cmdbuf, le16(&ihid->hdesc.wCommandRegister));
		length += 2;
		length += i2c_hid_encode_command(ihid->cmdbuf + length,
						 I2C_HID_OPCODE_SET_REPORT,
						 report_type, report_id);
		/* The report data goes to the data register. */
		put_le16(ihid->cmdbuf + length,
			 le16(&ihid->hdesc.wDataRegister));
		length += 2;
	} else {
		/* A plain output report: everything to the output
		 * register. */
		put_le16(ihid->cmdbuf, le16(&ihid->hdesc.wOutputRegister));
		length += 2;
	}
	length += i2c_hid_format_report(ihid->cmdbuf + length, report_id, buf,
					data_len);

	error = i2c_hid_xfer(ihid, ihid->cmdbuf, (int)length, NULL, 0);
	mutex_unlock(&ihid->cmd_lock);
	if (error) {
		ihid_err(ihid, "failed to set a report to device: %d\n", error);
		return error;
	}
	return (int)data_len;
}

static int i2c_hid_set_power_command(struct i2c_hid *ihid, int power_state)
{
	size_t length;
	int ret;

	mutex_lock(&ihid->cmd_lock);
	put_le16(ihid->cmdbuf, le16(&ihid->hdesc.wCommandRegister));
	length = 2;
	length += i2c_hid_encode_command(ihid->cmdbuf + length,
					 I2C_HID_OPCODE_SET_POWER, 0,
					 power_state);
	ret = i2c_hid_xfer(ihid, ihid->cmdbuf, (int)length, NULL, 0);
	mutex_unlock(&ihid->cmd_lock);
	return ret;
}

static int i2c_hid_set_power(struct i2c_hid *ihid, int power_state)
{
	int ret;

	might_sleep();
	/* A device in deep sleep may not acknowledge its address on the
	 * first request (STM-based devices need 400 us after a rising clock
	 * edge, so do certain Weida Tech devices): try once more after a
	 * short sleep. */
	ret = i2c_hid_set_power_command(ihid, power_state);
	if (ret && power_state == I2C_HID_PWR_ON) {
		usleep_range(400, 500);
		ret = i2c_hid_set_power_command(ihid, I2C_HID_PWR_ON);
	}
	if (ret)
		ihid_err(ihid, "failed to change power setting.\n");

	/* The specification says a device that needs time after PWR_ON
	 * stretches the clock, but Windows waits between PWR_ON and RESET
	 * (and some vendors' drivers 60 ms after PWR_ON), and several
	 * devices do not work without that delay. */
	if (!ret && power_state == I2C_HID_PWR_ON)
		msleep(60);
	return ret;
}

/* Power on and send RESET; the acknowledgement is waited for by
 * i2c_hid_finish_hwreset().  reset_lock held: no feature request may be
 * sent while the device resets, or the reset-done interrupt is lost. */
static int i2c_hid_start_hwreset(struct i2c_hid *ihid)
{
	size_t length = 0;
	int ret;

	mutex_assert_held(&ihid->reset_lock);
	ret = i2c_hid_set_power(ihid, I2C_HID_PWR_ON);
	if (ret)
		return ret;

	mutex_lock(&ihid->cmd_lock);
	put_le16(ihid->cmdbuf, le16(&ihid->hdesc.wCommandRegister));
	length += 2;
	length += i2c_hid_encode_command(ihid->cmdbuf + length,
					 I2C_HID_OPCODE_RESET, 0, 0);
	ihid_set(ihid, I2C_HID_RESET_PENDING);
	ret = i2c_hid_xfer(ihid, ihid->cmdbuf, (int)length, NULL, 0);
	mutex_unlock(&ihid->cmd_lock);
	if (!ret)
		return 0;

	ihid_err(ihid, "failed to reset device: %d\n", ret);
	/* Clean up after the failed reset command. */
	ihid_clear(ihid, I2C_HID_RESET_PENDING);
	i2c_hid_set_power(ihid, I2C_HID_PWR_SLEEP);
	return ret;
}

static int i2c_hid_finish_hwreset(struct i2c_hid *ihid)
{
	int ret = 0;

	mutex_assert_held(&ihid->reset_lock);
	if (ihid->quirks & I2C_HID_QUIRK_NO_IRQ_AFTER_RESET) {
		msleep(100);
		ihid_clear(ihid, I2C_HID_RESET_PENDING);
	} else if (!wait_event_timeout(ihid->wait,
				       !ihid_test(ihid, I2C_HID_RESET_PENDING),
				       I2C_HID_RESET_TIMEOUT_MS)) {
		ihid_warn(ihid, "device did not ack reset within %d ms\n",
			  I2C_HID_RESET_TIMEOUT_MS);
		ihid_clear(ihid, I2C_HID_RESET_PENDING);
	}

	/* At least some SIS devices need this after reset. */
	if (!(ihid->quirks & I2C_HID_QUIRK_NO_WAKEUP_AFTER_RESET))
		ret = i2c_hid_set_power(ihid, I2C_HID_PWR_ON);
	return ret;
}

// ---------------------------------------------------------------------------
// Recovery
// ---------------------------------------------------------------------------

/* The HID driver's reset_resume: it forgets the contacts that were down
 * (reporting their release) and sends its configuration again.  The
 * interrupt is held off meanwhile, so the release cannot interleave with a
 * report the interrupt thread is handing to the same driver.  Process
 * context, no transport lock held. */
static int i2c_hid_reset_resume_driver(struct i2c_hid *ihid)
{
	int ret;

	might_sleep();
	disable_irq(ihid->irq);
	ret = hid_driver_reset_resume(ihid->hid);
	enable_irq(ihid->irq);
	return ret;
}

/* Ask the work item for a reset (`bit' I2C_HID_RECOVER_RESET: the
 * interrupt is masked until then, a failing device on a level-triggered
 * line would otherwise keep the thread busy) or for re-sending the HID
 * driver's configuration (I2C_HID_RECOVER_REINIT).  Interrupt thread. */
static void i2c_hid_schedule_recovery(struct i2c_hid *ihid, int bit)
{
	if (!ihid_test(ihid, I2C_HID_ADDED) ||
	    ihid_test(ihid, I2C_HID_REMOVING) ||
	    ihid_test(ihid, I2C_HID_SUSPENDED) || ihid_test(ihid, I2C_HID_DEAD))
		return;
	/* One at a time: a recovery in progress (whose reset makes the
	 * device send its acknowledgement) is not disturbed, and a reset
	 * re-sends the configuration anyway. */
	if (ihid_test_and_set(ihid, I2C_HID_RECOVERY_BUSY))
		return;
	ihid_set(ihid, bit);
	if (bit == I2C_HID_RECOVER_RESET &&
	    !ihid_test_and_set(ihid, I2C_HID_IRQ_OFF_RECOVERY))
		disable_irq_nosync(ihid->irq);
	queue_work(system_wq, &ihid->recovery_work);
}

/* One message per recovery window from the work item. */
static int ihid_ratelimit_work(struct i2c_hid *ihid)
{
	int first;

	mutex_lock(&ihid->recovery_lock);
	first = !ihid->recovery_warned;
	ihid->recovery_warned = 1;
	mutex_unlock(&ihid->recovery_lock);
	return first;
}

/* Room in the window for one more recovery? */
static int i2c_hid_recovery_allowed(struct i2c_hid *ihid)
{
	uint64_t now = timer_get_precise_ns();
	int ok;

	mutex_lock(&ihid->recovery_lock);
	if (!ihid->recovery_window_start ||
	    now - ihid->recovery_window_start > I2C_HID_RECOVERY_WINDOW_NS) {
		ihid->recovery_window_start = now ? now : 1;
		ihid->recovery_count = 0;
		ihid->recovery_warned = 0;
	}
	ok = ihid->recovery_count < I2C_HID_RECOVERY_MAX;
	if (ok)
		ihid->recovery_count++;
	mutex_unlock(&ihid->recovery_lock);
	return ok;
}

static void i2c_hid_recovery_work(struct work_struct *work)
{
	struct i2c_hid *ihid =
		(struct i2c_hid *)((char *)work -
				   __builtin_offsetof(struct i2c_hid,
						      recovery_work));
	int do_reset = ihid_test_and_clear(ihid, I2C_HID_RECOVER_RESET);
	int do_reinit = ihid_test_and_clear(ihid, I2C_HID_RECOVER_REINIT);
	int ret;

	might_sleep();
	if (ihid_test(ihid, I2C_HID_REMOVING) ||
	    ihid_test(ihid, I2C_HID_SUSPENDED))
		return; // the canceller balances the interrupt
	if (!do_reset && !do_reinit) {
		ihid_clear(ihid, I2C_HID_RECOVERY_BUSY);
		return;
	}

	if (!i2c_hid_recovery_allowed(ihid)) {
		if (do_reset) {
			/* It keeps failing: leave it alone, its interrupt
			 * masked, until the next resume. */
			if (!ihid_test_and_set(ihid, I2C_HID_DEAD))
				ihid_err(ihid,
					 "too many errors, device disabled until the next resume\n");
			/* RECOVERY_BUSY stays set: nothing more is tried
			 * until the next resume clears it. */
		} else {
			/* A device that keeps resetting itself still works:
			 * it just is not configured again for a while. */
			if (ihid_ratelimit_work(ihid))
				ihid_warn(ihid,
					  "device keeps resetting itself, not configuring it again for now\n");
			ihid_clear(ihid, I2C_HID_RECOVERY_BUSY);
		}
		return;
	}

	if (do_reset) {
		ihid_warn(ihid, "repeated I/O errors, resetting the device\n");
		/* The reset acknowledgement comes as an interrupt. */
		if (ihid_test_and_clear(ihid, I2C_HID_IRQ_OFF_RECOVERY))
			enable_irq(ihid->irq);
		mutex_lock(&ihid->reset_lock);
		ret = i2c_hid_start_hwreset(ihid);
		if (!ret)
			ret = i2c_hid_finish_hwreset(ihid);
		mutex_unlock(&ihid->reset_lock);
		if (ret) {
			ihid_err(ihid, "reset after I/O errors failed: %d\n",
				 ret);
			/* The next errors may try again (within the
			 * window's budget). */
			ihid_clear(ihid, I2C_HID_RECOVERY_BUSY);
			return;
		}
		ihid_stat_inc(&ihid->stats.recoveries);
		ihid_info(ihid, "device reset, configuring it again\n");
	} else {
		ihid_info(ihid, "device reset itself, configuring it again\n");
		ihid_stat_inc(&ihid->stats.reinits);
	}
	/* The device lost its configuration (input mode, latency): the HID
	 * driver sends it again. */
	ret = i2c_hid_reset_resume_driver(ihid);
	if (ret)
		ihid_err(ihid, "configuring the device again failed: %d\n",
			 ret);
	ihid_clear(ihid, I2C_HID_RECOVERY_BUSY);
}

/* Stop the work for good (suspend, remove) and give the interrupt back if
 * the work had it masked.  The callers set SUSPENDED or REMOVING first and
 * let the interrupt thread finish, so nothing queues it again. */
static void i2c_hid_cancel_recovery(struct i2c_hid *ihid)
{
	might_sleep();
	cancel_work_sync(&ihid->recovery_work);
	ihid_clear(ihid, I2C_HID_RECOVER_RESET);
	ihid_clear(ihid, I2C_HID_RECOVER_REINIT);
	ihid_clear(ihid, I2C_HID_RECOVERY_BUSY);
	if (ihid_test_and_clear(ihid, I2C_HID_IRQ_OFF_RECOVERY))
		enable_irq(ihid->irq);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

/* An input read failed or delivered garbage. */
static void i2c_hid_input_error(struct i2c_hid *ihid, int weight)
{
	/* The reset of a recovery in progress produces errors of its own
	 * (the device is not ready yet): they count for nothing. */
	if (ihid_test(ihid, I2C_HID_RECOVERY_BUSY))
		return;
	ihid->io_errors += (unsigned int)weight;
	if (ihid->io_errors >= I2C_HID_RECOVERY_ERRORS) {
		ihid->io_errors = 0;
		i2c_hid_schedule_recovery(ihid, I2C_HID_RECOVER_RESET);
	}
}

static void i2c_hid_get_input(struct i2c_hid *ihid)
{
	uint16_t size = le16(&ihid->hdesc.wMaxInputLength);
	uint16_t ret_size;
	int ret;

	/* inbuf is only replaced with the interrupt disabled. */
	if (size > ihid->bufsize)
		size = (uint16_t)ihid->bufsize;
	if (WARN_ON_ONCE(size < 2))
		return; // refused at probe; never reached

	ret = i2c_master_recv(ihid->client, (char *)ihid->inbuf, size);
	if (ret != size) {
		ihid_stat_inc(&ihid->stats.read_errors);
		if (ret < 0) {
			/* Asleep (suspend raced) or gone: not counted while
			 * suspending or removing. */
			if (ihid_test(ihid, I2C_HID_SUSPENDED) ||
			    ihid_test(ihid, I2C_HID_REMOVING))
				return;
			if (ihid_ratelimit(ihid))
				ihid_err(ihid, "i2c_hid_get_input: read failed: %d\n",
					 ret);
			i2c_hid_input_error(ihid, ret == -ETIMEDOUT ?
							  I2C_HID_TIMEOUT_WEIGHT :
							  1);
			return;
		}
		if (ihid_ratelimit(ihid))
			ihid_err(ihid, "i2c_hid_get_input: got %d data instead of %d\n",
				 ret, size);
		i2c_hid_input_error(ihid, 1);
		return;
	}

	ret_size = le16(ihid->inbuf);
	if (!ret_size) {
		/* A host- or device-initiated RESET completed. */
		if (ihid_test_and_clear(ihid, I2C_HID_RESET_PENDING)) {
			ihid_stat_inc(&ihid->stats.resets_acked);
			wake_up(&ihid->wait);
		} else {
			ihid_stat_inc(&ihid->stats.spurious);
			/* The device reset on its own and forgot what the
			 * HID driver configured. */
			if (ihid_test(ihid, I2C_HID_HW_STARTED))
				i2c_hid_schedule_recovery(ihid,
							  I2C_HID_RECOVER_REINIT);
		}
		ihid->io_errors = 0;
		return;
	}

	if ((ihid->quirks & I2C_HID_QUIRK_BOGUS_IRQ) && ret_size == 0xffff) {
		ihid_stat_inc(&ihid->stats.bogus);
		if (__atomic_load_n(&ihid->stats.bogus, __ATOMIC_RELAXED) == 1)
			ihid_warn(ihid,
				  "i2c_hid_get_input: IRQ triggered but there's no data\n");
		return;
	}

	if (ret_size > size || ret_size < 2) {
		if (ihid->quirks & I2C_HID_QUIRK_BAD_INPUT_SIZE) {
			put_le16(ihid->inbuf, size);
			ret_size = size;
		} else {
			ihid_stat_inc(&ihid->stats.bad_length);
			if (ihid_ratelimit(ihid))
				ihid_err(ihid,
					 "i2c_hid_get_input: incomplete report (%d/%d)\n",
					 size, ret_size);
			i2c_hid_input_error(ihid, 1);
			return;
		}
	}
	ihid->io_errors = 0;

	/* A report of nothing but its length: nothing to deliver. */
	if (ret_size == 2)
		return;
	if (ihid_test(ihid, I2C_HID_STARTED) &&
	    ihid_test(ihid, I2C_HID_HW_STARTED)) {
		ihid_stat_inc(&ihid->stats.reports);
		hid_input_report(ihid->hid, HID_INPUT_REPORT, ihid->inbuf + 2,
				 (uint32_t)(ret_size - 2), 1);
	}
}

/* The threaded interrupt handler: process context, the line masked until
 * it returns. */
static int i2c_hid_irq(void *dev_id)
{
	struct i2c_hid *ihid = dev_id;

	i2c_hid_get_input(ihid);
	return IRQ_HANDLED;
}

// ---------------------------------------------------------------------------
// Buffers
// ---------------------------------------------------------------------------

/* Bytes the report takes on the wire: length prefix, ID, data. */
static unsigned int i2c_hid_get_report_length(const struct hid_report *report)
{
	return (report->size + 7) / 8 +
	       report->device->report_enum[report->type].numbered + 2;
}

/* The longest report of a type (wMaxInputLength is not trusted: some
 * devices state a wrong one). */
static void i2c_hid_find_max_report(struct hid_device *hid, unsigned int type,
				    unsigned int *max)
{
	struct hid_report *report;

	list_for_each_entry(report, &hid->report_enum[type].report_list, list) {
		unsigned int size = i2c_hid_get_report_length(report);

		if (*max < size)
			*max = size;
	}
}

struct i2c_hid_bufs {
	uint8_t *inbuf, *rawbuf, *cmdbuf;
};

/* The worst case of cmdbuf is a SET_REPORT with a report ID of 15 or more
 * and the longest report. */
static int i2c_hid_alloc_bufs(struct i2c_hid_bufs *b, size_t report_size)
{
	size_t cmd_len = 2 + /* command register */
			 1 + /* encoded report type/ID */
			 1 + /* opcode */
			 1 + /* optional 3rd byte report ID */
			 2 + /* data register */
			 2 + /* report data size */
			 1 + /* report ID if numbered report */
			 report_size;

	b->inbuf = kcalloc(1, report_size);
	b->rawbuf = kcalloc(1, report_size);
	b->cmdbuf = kcalloc(1, cmd_len);
	if (!b->inbuf || !b->rawbuf || !b->cmdbuf) {
		kfree(b->inbuf);
		kfree(b->rawbuf);
		kfree(b->cmdbuf);
		return -ENOMEM;
	}
	return 0;
}

static void i2c_hid_free_buffers(struct i2c_hid *ihid)
{
	kfree(ihid->inbuf);
	kfree(ihid->rawbuf);
	kfree(ihid->cmdbuf);
	ihid->inbuf = NULL;
	ihid->rawbuf = NULL;
	ihid->cmdbuf = NULL;
	ihid->bufsize = 0;
}

// ---------------------------------------------------------------------------
// The HID core's transport operations
// ---------------------------------------------------------------------------

static struct i2c_hid *hid_to_ihid(struct hid_device *hid)
{
	struct i2c_client *client = hid->driver_data;

	return i2c_get_clientdata(client);
}

static int i2c_hid_get_raw_report(struct hid_device *hid, uint8_t report_type,
				  uint8_t report_id, uint8_t *buf, size_t count)
{
	struct i2c_hid *ihid = hid_to_ihid(hid);
	int ret_count;

	if (report_type == HID_OUTPUT_REPORT)
		return -EINVAL;
	if (!count)
		return -EINVAL;

	/* An unnumbered report's answer has no ID byte, but the caller
	 * expects one: put it there and read into the rest. */
	if (!report_id) {
		buf[0] = 0;
		buf++;
		count--;
	}

	mutex_lock(&ihid->reset_lock);
	ret_count = i2c_hid_get_report(ihid,
				       report_type == HID_FEATURE_REPORT ?
					       I2C_HID_TYPE_FEATURE :
					       I2C_HID_TYPE_INPUT,
				       report_id, buf, count);
	mutex_unlock(&ihid->reset_lock);

	if (ret_count > 0 && !report_id)
		ret_count++;
	return ret_count;
}

static int i2c_hid_output_raw_report(struct hid_device *hid,
				     uint8_t report_type, const uint8_t *buf,
				     size_t count, bool do_set)
{
	struct i2c_hid *ihid = hid_to_ihid(hid);
	int report_id;
	int ret;

	if (report_type == HID_INPUT_REPORT)
		return -EINVAL;
	if (!count)
		return -EINVAL;
	report_id = buf[0];

	mutex_lock(&ihid->reset_lock);
	/* Numbered and unnumbered reports both carry the report ID in the
	 * first byte here; it is stripped unconditionally and
	 * i2c_hid_set_or_send_report() puts it back where the protocol
	 * wants it. */
	ret = i2c_hid_set_or_send_report(ihid,
					 report_type == HID_FEATURE_REPORT ?
						 I2C_HID_TYPE_FEATURE :
						 I2C_HID_TYPE_OUTPUT,
					 (uint8_t)report_id, buf + 1, count - 1,
					 do_set);
	if (ret >= 0)
		ret++; // the report ID byte counts as sent
	mutex_unlock(&ihid->reset_lock);
	return ret;
}

static int i2c_hid_output_report(struct hid_device *hid, uint8_t *buf,
				 size_t count)
{
	return i2c_hid_output_raw_report(hid, HID_OUTPUT_REPORT, buf, count,
					 false);
}

static int i2c_hid_raw_request(struct hid_device *hid, unsigned char reportnum,
			       uint8_t *buf, size_t len, unsigned char rtype,
			       int reqtype)
{
	might_sleep();
	switch (reqtype) {
	case HID_REQ_GET_REPORT:
		return i2c_hid_get_raw_report(hid, rtype, reportnum, buf, len);
	case HID_REQ_SET_REPORT:
		if (!len || buf[0] != reportnum)
			return -EINVAL;
		return i2c_hid_output_raw_report(hid, rtype, buf, len, true);
	default:
		return -EIO;
	}
}

static int i2c_hid_parse(struct hid_device *hid)
{
	struct i2c_client *client = hid->driver_data;
	struct i2c_hid *ihid = i2c_get_clientdata(client);
	const uint8_t *use_override;
	uint8_t *rdesc = NULL;
	unsigned int rsize;
	int tries = 3;
	int ret;

	might_sleep();
	rsize = le16(&ihid->hdesc.wReportDescLength);
	if (!rsize || rsize > HID_MAX_DESCRIPTOR_SIZE) {
		ihid_err(ihid, "weird size of report descriptor (%u)\n", rsize);
		return -EINVAL;
	}

	mutex_lock(&ihid->reset_lock);
	do {
		ret = i2c_hid_start_hwreset(ihid);
		if (ret == 0)
			ret = i2c_hid_finish_hwreset(ihid);
		else
			msleep(1000);
	} while (tries-- > 0 && ret);
	mutex_unlock(&ihid->reset_lock);
	if (ret)
		return ret;

	use_override =
		i2c_hid_get_dmi_hid_report_desc_override(client->name, &rsize);
	if (use_override) {
		if (WARN_ON_ONCE(!rsize || rsize > HID_MAX_DESCRIPTOR_SIZE))
			return -EINVAL;
		ihid_info(ihid, "using a HID report descriptor override\n");
		ret = hid_parse_report(hid, use_override, rsize);
		if (ret)
			ihid_err(ihid, "parsing report descriptor failed\n");
		return ret;
	}

	rdesc = kcalloc(1, rsize);
	if (!rdesc)
		return -ENOMEM;
	ret = i2c_hid_read_register(ihid,
				    le16(&ihid->hdesc.wReportDescRegister),
				    rdesc, rsize);
	if (ret) {
		ihid_err(ihid, "reading report descriptor failed\n");
		kfree(rdesc);
		return ret;
	}
	ret = hid_parse_report(hid, rdesc, rsize);
	if (ret)
		ihid_err(ihid, "parsing report descriptor failed\n");
	kfree(rdesc);
	return ret;
}

static int i2c_hid_start(struct hid_device *hid)
{
	struct i2c_hid *ihid = hid_to_ihid(hid);
	unsigned int bufsize = I2C_HID_MIN_BUFFER_SIZE;
	struct i2c_hid_bufs nb, old;

	might_sleep();
	i2c_hid_find_max_report(hid, HID_INPUT_REPORT, &bufsize);
	i2c_hid_find_max_report(hid, HID_OUTPUT_REPORT, &bufsize);
	i2c_hid_find_max_report(hid, HID_FEATURE_REPORT, &bufsize);
	/* The HID core bounds every report; the transport's length field is
	 * 16 bits. */
	if (WARN_ON_ONCE(bufsize > 0xffff))
		return -EINVAL;

	if (bufsize > ihid->bufsize) {
		/* The new buffers first: a failure leaves the old ones. */
		if (i2c_hid_alloc_bufs(&nb, bufsize))
			return -ENOMEM;
		mutex_lock(&ihid->reset_lock);
		mutex_lock(&ihid->cmd_lock);
		/* The interrupt thread uses inbuf without a lock. */
		disable_irq(ihid->irq);
		old.inbuf = ihid->inbuf;
		old.rawbuf = ihid->rawbuf;
		old.cmdbuf = ihid->cmdbuf;
		ihid->inbuf = nb.inbuf;
		ihid->rawbuf = nb.rawbuf;
		ihid->cmdbuf = nb.cmdbuf;
		ihid->bufsize = bufsize;
		enable_irq(ihid->irq);
		mutex_unlock(&ihid->cmd_lock);
		mutex_unlock(&ihid->reset_lock);
		kfree(old.inbuf);
		kfree(old.rawbuf);
		kfree(old.cmdbuf);
	}
	ihid_set(ihid, I2C_HID_HW_STARTED);
	return 0;
}

/* No report reaches the HID core once this returns: the flags are clear
 * and a thread run that saw them set has finished. */
static void i2c_hid_stop(struct hid_device *hid)
{
	struct i2c_hid *ihid = hid_to_ihid(hid);

	might_sleep();
	ihid_clear(ihid, I2C_HID_HW_STARTED);
	ihid_clear(ihid, I2C_HID_STARTED);
	if (ihid->irq_requested)
		synchronize_irq(ihid->irq);
	hid->claimed = 0;
}

static int i2c_hid_open(struct hid_device *hid)
{
	struct i2c_hid *ihid = hid_to_ihid(hid);

	ihid_set(ihid, I2C_HID_STARTED);
	return 0;
}

static void i2c_hid_close(struct hid_device *hid)
{
	struct i2c_hid *ihid = hid_to_ihid(hid);

	ihid_clear(ihid, I2C_HID_STARTED);
}

static int i2c_hid_power(struct hid_device *hid, int level)
{
	/* The device manages its own power between reports; SET_POWER is
	 * only used around system sleep. */
	(void)hid;
	(void)level;
	return 0;
}

static bool i2c_hid_may_wakeup(struct hid_device *hid)
{
	struct i2c_client *client = hid->driver_data;

	return (client->flags & I2C_CLIENT_WAKE) != 0;
}

static const struct hid_ll_driver i2c_hid_ll_driver = {
	.parse = i2c_hid_parse,
	.start = i2c_hid_start,
	.stop = i2c_hid_stop,
	.open = i2c_hid_open,
	.close = i2c_hid_close,
	.power = i2c_hid_power,
	.output_report = i2c_hid_output_report,
	.raw_request = i2c_hid_raw_request,
	.may_wakeup = i2c_hid_may_wakeup,
	/* No max_buffer_size: the HID core's HID_MAX_BUFFER_SIZE bounds every
	 * report well inside the 16-bit length fields of the transport. */
};

// ---------------------------------------------------------------------------
// Probe
// ---------------------------------------------------------------------------

static int i2c_hid_init_irq(struct i2c_client *client)
{
	struct i2c_hid *ihid = i2c_get_clientdata(client);
	int ret;

	/* A line whose trigger firmware did not give is taken as level low,
	 * what the specification prescribes. */
	if (irq_get_trigger_type(client->irq) == IRQ_TYPE_NONE)
		(void)irq_set_irq_type(client->irq, IRQ_TYPE_LEVEL_LOW);

	ret = request_threaded_irq(client->irq, NULL, i2c_hid_irq,
				   IRQF_ONESHOT | IRQF_NO_AUTOEN, client->name,
				   ihid);
	if (ret < 0) {
		ihid_warn(ihid,
			  "Could not register for %s interrupt, irq = %d, ret = %d\n",
			  client->name, client->irq, ret);
		return ret;
	}
	ihid->irq = client->irq;
	ihid->irq_requested = 1;
	return 0;
}

static int i2c_hid_fetch_hid_descriptor(struct i2c_hid *ihid)
{
	struct i2c_client *client = ihid->client;
	const struct i2c_hid_desc *override;
	struct i2c_hid_desc *hdesc = &ihid->hdesc;
	unsigned int dsize;
	int error;

	BUILD_BUG_ON(sizeof(struct i2c_hid_desc) != I2C_HID_DESC_LENGTH);
	override = i2c_hid_get_dmi_i2c_hid_desc_override(client->name);
	if (override) {
		ihid_info(ihid, "using a HID descriptor override\n");
		ihid->hdesc = *override;
	} else {
		/* A fixed size: the length field is checked afterwards. */
		error = i2c_hid_read_register(ihid, ihid->wHIDDescRegister,
					      &ihid->hdesc,
					      sizeof(ihid->hdesc));
		if (error) {
			ihid_err(ihid, "failed to fetch HID descriptor: %d\n",
				 error);
			return -ENODEV;
		}
	}

	/* bcdVersion must be 1.00. */
	if (le16(&hdesc->bcdVersion) != 0x0100) {
		ihid_err(ihid, "unexpected HID descriptor bcdVersion (0x%04x)\n",
			 le16(&hdesc->bcdVersion));
		return -ENODEV;
	}
	/* The specification's 30 bytes. */
	dsize = le16(&hdesc->wHIDDescLength);
	if (dsize != sizeof(struct i2c_hid_desc)) {
		ihid_err(ihid, "weird size of HID descriptor (%u)\n", dsize);
		return -ENODEV;
	}
	/* The input register delivers the 2-byte length at least. */
	if (le16(&hdesc->wMaxInputLength) < 2) {
		ihid_err(ihid, "invalid wMaxInputLength (%u)\n",
			 le16(&hdesc->wMaxInputLength));
		return -ENODEV;
	}
	return 0;
}

static int i2c_hid_core_power_up(struct i2c_hid *ihid)
{
	if (!ihid->ops || !ihid->ops->power_up)
		return 0;
	return ihid->ops->power_up(ihid->ops);
}

static void i2c_hid_core_power_down(struct i2c_hid *ihid)
{
	if (!ihid->ops || !ihid->ops->power_down)
		return;
	ihid->ops->power_down(ihid->ops);
}

static void i2c_hid_core_shutdown_tail(struct i2c_hid *ihid)
{
	if (!ihid->ops || !ihid->ops->shutdown_tail)
		return;
	ihid->ops->shutdown_tail(ihid->ops);
}

/* Suspend: the HID driver first (it may still talk to the device), then
 * the device sleeps and its interrupt is masked. */
static int i2c_hid_core_suspend(struct i2c_hid *ihid, bool force_poweroff)
{
	struct i2c_client *client = ihid->client;
	int ret;

	might_sleep();
	if (ihid_test(ihid, I2C_HID_SUSPENDED))
		return 0;
	if (ihid_test(ihid, I2C_HID_ADDED)) {
		ret = hid_driver_suspend(ihid->hid, HID_PM_EVENT_SUSPEND);
		if (ret < 0)
			return ret;
	}

	/* No recovery from here on; a thread run that is deciding about one
	 * finishes first. */
	ihid_set(ihid, I2C_HID_SUSPENDED);
	synchronize_irq(ihid->irq);
	i2c_hid_cancel_recovery(ihid);

	/* Save some power. */
	if (!(ihid->quirks & I2C_HID_QUIRK_NO_SLEEP_ON_SUSPEND))
		i2c_hid_set_power(ihid, I2C_HID_PWR_SLEEP);

	disable_irq(ihid->irq);

	if (force_poweroff || !(client->flags & I2C_CLIENT_WAKE))
		i2c_hid_core_power_down(ihid);
	return 0;
}

static int i2c_hid_core_resume(struct i2c_hid *ihid)
{
	struct i2c_client *client = ihid->client;
	int ret;

	might_sleep();
	if (!ihid_test(ihid, I2C_HID_SUSPENDED))
		return 0;
	if (!(client->flags & I2C_CLIENT_WAKE))
		i2c_hid_core_power_up(ihid);

	/* A new start for a device given up on. */
	ihid_clear(ihid, I2C_HID_DEAD);
	mutex_lock(&ihid->recovery_lock);
	ihid->recovery_window_start = 0;
	ihid->recovery_count = 0;
	mutex_unlock(&ihid->recovery_lock);
	ihid_clear(ihid, I2C_HID_SUSPENDED);

	enable_irq(ihid->irq);

	/* Goodix 27c6:0d42 never sends input interrupts again if it is
	 * woken too early. */
	if (ihid->quirks & I2C_HID_QUIRK_DELAY_WAKEUP_AFTER_RESUME)
		msleep(1500);

	/* Instead of resetting the device, simply power it on: that solves
	 * "incomplete reports" on Raydium devices and SIS touchscreens that
	 * send nothing after a resume.  ALPS touchpads generate an interrupt
	 * storm without a reset, so those are still reset. */
	if (ihid->quirks & I2C_HID_QUIRK_RESET_ON_RESUME) {
		mutex_lock(&ihid->reset_lock);
		ret = i2c_hid_start_hwreset(ihid);
		if (ret == 0)
			ret = i2c_hid_finish_hwreset(ihid);
		mutex_unlock(&ihid->reset_lock);
	} else {
		ret = i2c_hid_set_power(ihid, I2C_HID_PWR_ON);
	}
	if (ret)
		return ret;
	if (!ihid_test(ihid, I2C_HID_ADDED))
		return 0;
	return i2c_hid_reset_resume_driver(ihid);
}

/* Something answers at the address, its HID descriptor is sane: fill in
 * the HID device's identity. */
static int __i2c_hid_core_probe(struct i2c_hid *ihid)
{
	struct i2c_client *client = ihid->client;
	struct hid_device *hid = ihid->hid;
	int ret;

	ret = i2c_hid_probe_address(ihid);
	if (ret < 0) {
		ihid_err(ihid, "nothing at this address: %d\n", ret);
		return -ENXIO;
	}

	ret = i2c_hid_fetch_hid_descriptor(ihid);
	if (ret < 0) {
		ihid_err(ihid, "Failed to fetch the HID Descriptor\n");
		return ret;
	}

	hid->version = le16(&ihid->hdesc.bcdVersion);
	hid->vendor = le16(&ihid->hdesc.wVendorID);
	hid->product = le16(&ihid->hdesc.wProductID);
	hid->initial_quirks |= i2c_hid_get_dmi_quirks((uint16_t)hid->vendor,
						      (uint16_t)hid->product);

	ksnprintf(hid->name, sizeof(hid->name), "%s %04X:%04X", client->name,
		  (unsigned int)(uint16_t)hid->vendor,
		  (unsigned int)(uint16_t)hid->product);
	ksnprintf(hid->phys, sizeof(hid->phys), "%s", client->dev_name);
	if (i2c_client_devpath(client, hid->parent_path,
			       sizeof(hid->parent_path)))
		hid->parent_path[0] = 0;

	ihid->quirks = i2c_hid_lookup_quirk((uint16_t)hid->vendor,
					    (uint16_t)hid->product);
	return 0;
}

static int i2c_hid_core_register_hid(struct i2c_hid *ihid)
{
	struct hid_device *hid = ihid->hid;
	int ret;

	enable_irq(ihid->irq);

	ret = hid_add_device(hid);
	if (ret) {
		if (ret != -ENODEV)
			ihid_err(ihid, "can't add hid device: %d\n", ret);
		disable_irq(ihid->irq);
		return ret;
	}
	ihid_set(ihid, I2C_HID_ADDED);

	/* At least some QTEC devices need this after initialization. */
	if (ihid->quirks & I2C_HID_QUIRK_RE_POWER_ON)
		ret = i2c_hid_set_power(ihid, I2C_HID_PWR_ON);
	return ret;
}

int i2c_hid_core_probe(struct i2c_client *client, struct i2chid_ops *ops,
		       uint16_t hid_descriptor_address, uint32_t quirks)
{
	struct i2c_hid_bufs b;
	struct i2c_hid *ihid;
	struct hid_device *hid;
	int ret;

	might_sleep();
	if (!client->irq) {
		kprintf("i2c_hid %s: HID over i2c has not been provided an Int IRQ\n",
			client->dev_name);
		return -EINVAL;
	}
	if (client->irq < 0) {
		if (client->irq != -EPROBE_DEFER)
			kprintf("i2c_hid %s: HID over i2c doesn't have a valid IRQ\n",
				client->dev_name);
		return client->irq;
	}

	ihid = kcalloc(1, sizeof(*ihid));
	if (!ihid)
		return -ENOMEM;
	ihid->ops = ops;
	ihid->client = client;
	ihid->irq = client->irq;
	ihid->wHIDDescRegister = hid_descriptor_address;
	wq_head_init(&ihid->wait, "i2c_hid_reset");
	mutex_init(&ihid->cmd_lock, "i2c_hid_cmd");
	mutex_init(&ihid->reset_lock, "i2c_hid_reset");
	mutex_init(&ihid->recovery_lock, "i2c_hid_recovery");
	INIT_WORK(&ihid->recovery_work, i2c_hid_recovery_work);

	/* The buffers before the report sizes are known; start() grows
	 * them. */
	ret = i2c_hid_alloc_bufs(&b, I2C_HID_MIN_BUFFER_SIZE);
	if (ret < 0) {
		kfree(ihid);
		return ret;
	}
	ihid->inbuf = b.inbuf;
	ihid->rawbuf = b.rawbuf;
	ihid->cmdbuf = b.cmdbuf;
	ihid->bufsize = I2C_HID_MIN_BUFFER_SIZE;
	i2c_set_clientdata(client, ihid);

	hid = hid_allocate_device();
	if (!hid) {
		ret = -ENOMEM;
		goto err_free_buffers;
	}
	ihid->hid = hid;
	hid->driver_data = client;
	hid->ll_driver = &i2c_hid_ll_driver;
	hid->bus = BUS_I2C;
	hid->initial_quirks = quirks;

	ret = i2c_hid_core_power_up(ihid);
	if (ret < 0)
		goto err_destroy_device;

	ret = __i2c_hid_core_probe(ihid);
	if (ret < 0)
		goto err_power_down;

	ret = i2c_hid_init_irq(client);
	if (ret < 0)
		goto err_power_down;

	ret = i2c_hid_core_register_hid(ihid);
	if (ret)
		goto err_free_irq;
	return 0;

err_free_irq:
	/* Registration failed (or, the HID device added, the power-on a
	 * quirk asks for afterwards): no recovery may start any more, the
	 * interrupt goes (waiting for a thread run in progress), then the HID
	 * device (its driver unbinds if it was added). */
	ihid_set(ihid, I2C_HID_REMOVING);
	free_irq(client->irq, ihid);
	ihid->irq_requested = 0;
	cancel_work_sync(&ihid->recovery_work);
	ihid_clear(ihid, I2C_HID_IRQ_OFF_RECOVERY);
	ihid_clear(ihid, I2C_HID_ADDED);
err_power_down:
	i2c_hid_core_power_down(ihid);
err_destroy_device:
	hid_destroy_device(hid);
	ihid->hid = NULL;
err_free_buffers:
	i2c_set_clientdata(client, NULL);
	i2c_hid_free_buffers(ihid);
	kfree(ihid);
	return ret;
}

void i2c_hid_core_remove(struct i2c_client *client)
{
	struct i2c_hid *ihid = i2c_get_clientdata(client);

	might_sleep();
	if (WARN_ON(!ihid))
		return;
	/* Quiet the device (HID driver suspend, SLEEP, interrupt masked);
	 * after a shutdown the interrupt is gone already. */
	if (ihid->irq_requested)
		i2c_hid_core_suspend(ihid, true);
	ihid_set(ihid, I2C_HID_REMOVING);
	if (ihid->irq_requested)
		synchronize_irq(ihid->irq);
	i2c_hid_cancel_recovery(ihid);

	/* The HID driver unbinds (stop: no report runs or starts after it),
	 * the input devices go. */
	hid_destroy_device(ihid->hid);
	ihid->hid = NULL;

	if (ihid->irq_requested) {
		free_irq(client->irq, ihid);
		ihid->irq_requested = 0;
	}
	/* The work cannot be queued any more (REMOVING), nor run. */
	cancel_work_sync(&ihid->recovery_work);
	i2c_set_clientdata(client, NULL);
	i2c_hid_free_buffers(ihid);
	kfree(ihid);
}

void i2c_hid_core_shutdown(struct i2c_client *client)
{
	struct i2c_hid *ihid = i2c_get_clientdata(client);

	might_sleep();
	if (!ihid)
		return;
	ihid_set(ihid, I2C_HID_REMOVING);
	if (ihid->irq_requested)
		synchronize_irq(ihid->irq);
	i2c_hid_cancel_recovery(ihid);
	i2c_hid_set_power(ihid, I2C_HID_PWR_SLEEP);
	if (ihid->irq_requested) {
		free_irq(client->irq, ihid);
		ihid->irq_requested = 0;
	}
	i2c_hid_core_shutdown_tail(ihid);
}

int i2c_hid_core_pm_suspend(struct i2c_client *client)
{
	struct i2c_hid *ihid = i2c_get_clientdata(client);

	if (!ihid || !ihid->irq_requested)
		return 0;
	return i2c_hid_core_suspend(ihid, false);
}

int i2c_hid_core_pm_resume(struct i2c_client *client)
{
	struct i2c_hid *ihid = i2c_get_clientdata(client);

	if (!ihid || !ihid->irq_requested)
		return 0;
	return i2c_hid_core_resume(ihid);
}

int i2c_hid_get_stats(struct i2c_client *client, struct i2c_hid_stats *out)
{
	struct i2c_hid *ihid = client ? i2c_get_clientdata(client) : NULL;

	if (!ihid || !out)
		return -ENODEV;
	*out = ihid->stats;
	return 0;
}

long i2c_hid_core_show(struct i2c_client *client, char *buf, long cap,
		       long pos)
{
	struct i2c_hid *ihid = client ? i2c_get_clientdata(client) : NULL;
	struct i2c_hid_stats st;
	unsigned int budget;
	char line[256];
	long n;

	if (!ihid)
		return pos;
	st = ihid->stats;
	mutex_lock(&ihid->recovery_lock);
	budget = ihid->recovery_count < I2C_HID_RECOVERY_MAX ?
			 I2C_HID_RECOVERY_MAX - ihid->recovery_count :
			 0;
	mutex_unlock(&ihid->recovery_lock);
	n = ksnprintf(line, sizeof(line),
		      "    i2c-hid: reports %lu read-errors %lu resets-acked %lu device-resets %lu reconfigured %lu spurious %lu bogus %lu bad-length %lu recovery-budget %u%s%s\n",
		      st.reports, st.read_errors, st.resets_acked,
		      st.recoveries, st.reinits, st.spurious, st.bogus,
		      st.bad_length, budget,
		      ihid_test(ihid, I2C_HID_DEAD) ? " disabled-until-resume" :
						      "",
		      ihid_test(ihid, I2C_HID_SUSPENDED) ? " suspended" : "");
	if (n < 0)
		return pos;
	if (n >= (long)sizeof(line))
		n = (long)sizeof(line) - 1;
	/* As the /proc printf does: what fits, the position advanced by
	 * all of it. */
	for (long i = 0; i < n; i++)
		if (pos + i < cap)
			buf[pos + i] = line[i];
	return pos + n;
}

struct i2chid_ops *i2c_hid_core_ops(struct i2c_client *client)
{
	struct i2c_hid *ihid = client ? i2c_get_clientdata(client) : NULL;

	return ihid ? ihid->ops : NULL;
}

struct hid_device *i2c_hid_get_hid(struct i2c_client *client)
{
	struct i2c_hid *ihid = client ? i2c_get_clientdata(client) : NULL;

	return ihid ? ihid->hid : NULL;
}
