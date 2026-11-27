/* Host tests of the I2C core and the DesignWare driver (host/test-i2c.sh):
 * timing formulas, register access modes, every kind of transfer against
 * the bus model (combined with repeated START, 10-bit, long, received
 * length, SMBus with PEC), errors (NACK, lost arbitration, timeouts,
 * zero-length quirk), interrupt sharing and a vanished controller,
 * concurrency on the bus lock, suspend races, runtime power, bus
 * recovery (controller bus clear and GPIOs), the HP ProBook / Dell Pro
 * ACPI and PCI shapes with ACPI client enumeration, naming and deferred
 * probe, AMD/ACPI platform controllers, /dev/i2c-N, the GenericSerialBus
 * region handler, the core's own rules, a randomised stress run, and
 * (test_arb.inc) the firmware bus arbitration, DMI quirks, bus lock
 * operations and the suspend test chain.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include "host/kshim/acpica_mock.h"
#include "host/i2c/model.h"
#include "host/i2c/mocks.h"
#include <kernel/dev/i2c/i2c.h>
#include <kernel/dev/i2c/i2c-core.h>
#include <kernel/dev/i2c/busses/i2c-designware-core.h>
#include <kernel/hal/acpi_dev.h>
#include <kernel/ke/workqueue.h>
#include <kernel/ke/irq.h>
#include <kernel/fs/device.h>

#ifndef TEST_FILTER
#define TEST_FILTER ""
#endif

union rslot {
	ACPI_RESOURCE r;
	UINT8 pad[sizeof(ACPI_RESOURCE) + 32];
};

/* ioctls of /dev/i2c-N and their argument layouts as user space has them */
#define U_I2C_RETRIES 0x0701
#define U_I2C_TIMEOUT 0x0702
#define U_I2C_SLAVE 0x0703
#define U_I2C_TENBIT 0x0704
#define U_I2C_FUNCS 0x0705
#define U_I2C_SLAVE_FORCE 0x0706
#define U_I2C_RDWR 0x0707
#define U_I2C_PEC 0x0708
#define U_I2C_SMBUS 0x0720
struct u_rdwr {
	struct i2c_msg *msgs;
	uint32_t nmsgs;
};
struct u_smbus {
	uint8_t read_write;
	uint8_t command;
	uint32_t size;
	union i2c_smbus_data *data;
};

static int str_eq(const char *a, const char *b)
{
	return !kstrcmp(a, b);
}

static int str_has(const char *hay, const char *needle)
{
	size_t n = kstrlen(needle);

	for (; *hay; hay++)
		if (!kstrncmp(hay, needle, n))
			return 1;
	return 0;
}

#define T_STR(a, b)                                                       \
	do {                                                              \
		if (!str_eq((a), (b))) {                                  \
			kprintf("  got      \"%s\"\n  expected \"%s\"\n", \
				(a), (b));                                \
			test_bug("T_STR", __FILE__, __LINE__, __func__);  \
		}                                                         \
	} while (0)

/* Warnings a test provokes on purpose. */
#define EXPECT_WARNS(w0, n)                                               \
	do {                                                              \
		T_EQ(g_test_warnings - (w0), (n));                        \
		__atomic_sub_fetch(&g_test_warnings, (n), __ATOMIC_RELAXED); \
	} while (0)

static void wait_ms(unsigned int ms)
{
	h_usleep(ms * 1000ul);
}

/* ==========================================================================
 * Setup helpers
 * ========================================================================== */

static uint32_t fixed_clk(struct dw_i2c_dev *dev)
{
	return dev->clk_khz;
}

/* A controller on `m' outside PCI and ACPI. */
static struct dw_i2c_dev *dw_new_ex(struct dw_model *m, uint32_t clk_khz,
				    uint32_t bus_hz, int polled,
				    struct acpi_device *adev)
{
	struct dw_i2c_dev *dev = kcalloc(1, sizeof(*dev));
	static int n;

	T_ASSERT(dev);
	ksnprintf(dev->name, sizeof(dev->name), "i2c_designware test%d", n++);
	dev->base = m->window;
	dev->clk_khz = clk_khz;
	dev->get_clk_rate_khz = clk_khz ? fixed_clk : NULL;
	dev->timings.bus_freq_hz = bus_hz;
	dev->irq = polled ? 0 : mock_irq_new(m);
	if (!polled)
		m->irq = dev->irq;
	dev->irq_kind = "test";
	dev->adapter.nr = -1;
	dev->acpi_dev = adev;
	if (m->lpss) {
		static uint32_t ctx[8][LPSS_PRIV_REG_COUNT];
		static int k;

		dev->lpss_priv = m->window + LPSS_PRIV_OFFSET_PCI;
		dev->glue = ctx[k++ & 7];
		dev->glue_suspend = i2c_dw_lpss_suspend;
		dev->glue_resume = i2c_dw_lpss_resume;
		i2c_dw_lpss_init_priv(dev, (uint64_t)(unsigned long)m->window);
	}
	T_EQ(i2c_dw_fw_parse_and_configure(dev), 0);
	i2c_dw_configure_master(dev);
	T_EQ(i2c_dw_probe(dev), 0);
	return dev;
}

static struct dw_i2c_dev *dw_new(struct dw_model *m, uint32_t clk_khz,
				 uint32_t bus_hz)
{
	return dw_new_ex(m, clk_khz, bus_hz, 0, NULL);
}

static void dw_del(struct dw_i2c_dev *dev)
{
	int irq = dev->irq;

	i2c_dw_remove(dev);
	if (irq > 0)
		irq_free_desc(irq);
	kfree(dev);
}

/* The usual targets: an EEPROM-like device at 0x50, an HID-like one at
 * 0x15 with a 30-byte descriptor at register 1, an SMBus device at 0x60,
 * a 10-bit device at 0x2a5, a stretching one at 0x40, one that NACKs
 * its second data byte at 0x34. */
static struct dwm_slave *g_ee, *g_hid, *g_smb, *g_ten, *g_slow, *g_nak;

static const uint8_t g_hid_desc[30] = {
	0x1e, 0x00, 0x00, 0x01, 0xb3, 0x01, 0x02, 0x00, 0x03, 0x00,
	0x20, 0x00, 0x04, 0x00, 0x11, 0x00, 0x05, 0x00, 0x06, 0x00,
	0xf3, 0x04, 0x69, 0x31, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static struct dw_model *model_std(unsigned int depth)
{
	struct dw_model *m = dwm_create(depth, depth);

	g_ee = dwm_slave_new(SLV_REGMEM, 0x50, 0);
	g_hid = dwm_slave_new(SLV_HID, 0x15, 0);
	kmemcpy(&g_hid->mem[1], g_hid_desc, sizeof(g_hid_desc));
	g_smb = dwm_slave_new(SLV_SMBUS, 0x60, 0);
	g_ten = dwm_slave_new(SLV_REGMEM, 0x2a5, 1);
	g_slow = dwm_slave_new(SLV_REGMEM, 0x40, 0);
	g_nak = dwm_slave_new(SLV_REGMEM, 0x34, 0);
	g_nak->nack_after = 2;
	dwm_add_slave(m, g_ee);
	dwm_add_slave(m, g_hid);
	dwm_add_slave(m, g_smb);
	dwm_add_slave(m, g_ten);
	dwm_add_slave(m, g_slow);
	dwm_add_slave(m, g_nak);
	return m;
}

/* Write `w' then (when rl) read `rl' bytes: i2c_transfer's result. */
static int wr(struct i2c_adapter *a, uint16_t addr, uint16_t flags,
	      const uint8_t *w, int wl, uint8_t *r, int rl)
{
	struct i2c_msg msg[2];
	int n = 0;

	if (wl || !rl) {
		msg[n].addr = addr;
		msg[n].flags = flags;
		msg[n].len = (uint16_t)wl;
		msg[n].buf = (uint8_t *)w;
		n++;
	}
	if (rl) {
		msg[n].addr = addr;
		msg[n].flags = (uint16_t)(flags | I2C_M_RD);
		msg[n].len = (uint16_t)rl;
		msg[n].buf = r;
		n++;
	}
	return i2c_transfer(a, msg, n);
}

static char g_log[8192];

static const char *bus_log(struct dw_model *m)
{
	dwm_log_text(m, g_log, sizeof(g_log));
	return g_log;
}

/* ==========================================================================
 * Timing formulas
 * ========================================================================== */

struct cnt_case {
	uint32_t clk, sda_fall, scl_fall;
	uint32_t ss_h, ss_l, fs_h, fs_l, fp_h, fp_l, hs_h, hs_l;
};

static void t_formulas(void)
{
	/* Expected values worked out by hand from
	 *   HCNT = round(clk * (tHIGH + tf) / 1e6) - 3,
	 *   LCNT = round(clk * (tLOW + tf) / 1e6) - 1
	 * with tHIGH/tLOW 4000/4700 (SS), 600/1300 (FS), 260/500 (FM+),
	 * 60/160 (HS, 100 pF). */
	static const struct cnt_case c[] = {
		{ 100000, 300, 300, 427, 499, 87, 159, 53, 79, 33, 45 },
		{ 120000, 300, 300, 513, 599, 105, 191, 64, 95, 40, 54 },
		{ 133000, 171, 208, 552, 652, 100, 200, 54, 93, 28, 48 },
		{ 150000, 300, 300, 642, 749, 132, 239, 81, 119, 51, 68 },
		{ 216000, 300, 300, 926, 1079, 191, 345, 118, 172, 75, 98 },
	};

	for (unsigned int i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
		T_EQ(i2c_dw_scl_hcnt(NULL, 0, c[i].clk, 4000, c[i].sda_fall, 0),
		     c[i].ss_h);
		T_EQ(i2c_dw_scl_lcnt(NULL, 0, c[i].clk, 4700, c[i].scl_fall, 0),
		     c[i].ss_l);
		T_EQ(i2c_dw_scl_hcnt(NULL, 0, c[i].clk, 600, c[i].sda_fall, 0),
		     c[i].fs_h);
		T_EQ(i2c_dw_scl_lcnt(NULL, 0, c[i].clk, 1300, c[i].scl_fall, 0),
		     c[i].fs_l);
		T_EQ(i2c_dw_scl_hcnt(NULL, 0, c[i].clk, 260, c[i].sda_fall, 0),
		     c[i].fp_h);
		T_EQ(i2c_dw_scl_lcnt(NULL, 0, c[i].clk, 500, c[i].scl_fall, 0),
		     c[i].fp_l);
		T_EQ(i2c_dw_scl_hcnt(NULL, 0, c[i].clk, 60, c[i].sda_fall, 0),
		     c[i].hs_h);
		T_EQ(i2c_dw_scl_lcnt(NULL, 0, c[i].clk, 160, c[i].scl_fall, 0),
		     c[i].hs_l);
	}
	/* offsets are added */
	T_EQ(i2c_dw_scl_hcnt(NULL, 0, 100000, 600, 300, 2), 89);
	T_EQ(i2c_dw_scl_lcnt(NULL, 0, 100000, 1300, 300, -1), 158);
	/* CRC-8 of "123456789" (polynomial 0x07) is 0xf4 */
	T_EQ(i2c_smbus_pec(0, (const uint8_t *)"123456789", 9), 0xf4);
}

/* Through a whole probe: the registers the controller is programmed
 * with for each speed, SDA hold and FIFO thresholds. */
static void t_probe_timings(void)
{
	static const struct {
		uint32_t clk, bus;
		uint32_t con_speed;
		uint32_t ss_h, ss_l, fs_h, fs_l, hs_h, hs_l;
	} c[] = {
		{ 100000, 100000, 1, 427, 499, 87, 159, 0, 0 },
		{ 120000, 400000, 2, 513, 599, 105, 191, 0, 0 },
		{ 133000, 1000000, 2, 0, 0, 0, 0, 0, 0 },
		{ 150000, 3400000, 3, 642, 749, 132, 239, 51, 68 },
	};

	for (unsigned int i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
		struct dw_model *m = model_std(32);
		struct dw_i2c_dev *dev = dw_new(m, c[i].clk, c[i].bus);

		T_EQ((m->con >> 1) & 3, c[i].con_speed);
		T_EQ(m->con & 0x61, 0x61); /* master, restart, slave off */
		if (c[i].bus == 1000000) {
			/* FM+ in the fast registers: 133 MHz, 300 ns */
			T_EQ(m->fs_hcnt, i2c_dw_scl_hcnt(NULL, 0, 133000, 260,
							 300, 0));
			T_EQ(m->fs_lcnt, i2c_dw_scl_lcnt(NULL, 0, 133000, 500,
							 300, 0));
		} else {
			T_EQ(m->ss_hcnt, c[i].ss_h);
			T_EQ(m->ss_lcnt, c[i].ss_l);
			T_EQ(m->fs_hcnt, c[i].fs_h);
			T_EQ(m->fs_lcnt, c[i].fs_l);
			T_EQ(m->hs_hcnt, c[i].hs_h);
			T_EQ(m->hs_lcnt, c[i].hs_l);
		}
		/* v1.21: hold kept as found (1), RX hold added */
		T_EQ(m->sda_hold, 0x10001);
		T_EQ(m->tx_tl, 16);
		T_EQ(m->rx_tl, 0);
		T_EQ(dev->tx_fifo_depth, 32);
		T_EQ(dev->adapter.bus_freq_hz, c[i].bus);
		T_EQ(dev->adapter.retries, 3);
		T_ASSERT(i2c_check_functionality(&dev->adapter,
						 I2C_FUNC_I2C |
							 I2C_FUNC_10BIT_ADDR |
							 I2C_FUNC_SMBUS_BLOCK_DATA));
		T_ASSERT(!i2c_check_functionality(&dev->adapter,
						  I2C_FUNC_SMBUS_QUICK));
		dw_del(dev);
		dwm_destroy(m);
	}
	/* no high speed in the controller: fast mode instead */
	{
		struct dw_model *m = model_std(8);
		struct dw_i2c_dev *dev;

		m->param1 &= ~(3u << 2);
		m->param1 |= 2u << 2;
		dev = dw_new(m, 100000, 3400000);
		T_EQ((m->con >> 1) & 3, 2);
		T_EQ(dev->timings.bus_freq_hz, 400000);
		T_EQ(dev->hs_hcnt, 0);
		dw_del(dev);
		dwm_destroy(m);
	}
	/* an old controller (v1.10): no SDA hold programming */
	{
		struct dw_model *m = model_std(8);
		struct dw_i2c_dev *dev;

		m->version = 0x3131302Au;
		dev = dw_new(m, 100000, 400000);
		T_EQ(dev->sda_hold_time, 0);
		T_EQ(m->sda_hold, 1);
		dw_del(dev);
		dwm_destroy(m);
	}
	/* no clock: firmware's counts kept */
	{
		struct dw_model *m = model_std(8);
		struct dw_i2c_dev *dev;

		m->ss_hcnt = 0x1f4;
		m->ss_lcnt = 0x24c;
		m->fs_hcnt = 0x4b;
		m->fs_lcnt = 0xa3;
		dev = dw_new(m, 0, 400000);
		T_EQ(m->fs_hcnt, 0x4b);
		T_EQ(m->fs_lcnt, 0xa3);
		T_EQ(m->ss_hcnt, 0x1f4);
		dw_del(dev);
		dwm_destroy(m);
	}
	/* an unsupported bus speed fails the probe configuration */
	{
		struct dw_i2c_dev d;

		kmemset(&d, 0, sizeof(d));
		i2c_strlcpy(d.name, "t", sizeof(d.name));
		d.timings.bus_freq_hz = 250000;
		T_EQ(i2c_dw_fw_parse_and_configure(&d), -EINVAL);
	}
}

/* ==========================================================================
 * Register access modes
 * ========================================================================== */

static void t_regaccess(void)
{
	static const int modes[] = { DWM_ACC_32, DWM_ACC_SWAP, DWM_ACC_16 };

	for (int i = 0; i < 3; i++) {
		struct dw_model *m = model_std(16);
		struct dw_i2c_dev *dev;
		uint8_t buf[4] = { 0x20, 1, 2, 3 }, rd[3];

		m->access = modes[i];
		dev = dw_new(m, 100000, 400000);
		T_EQ((int)dev->access, i);
		T_EQ(dev->comp_version, 0x3132312A);
		T_EQ(dev->tx_fifo_depth, 16);
		T_EQ(m->fs_hcnt, 87);
		T_EQ(m->sda_hold, 0x10001);
		/* a transfer through the mode */
		T_EQ(wr(&dev->adapter, 0x50, 0, buf, 4, NULL, 0), 1);
		T_EQ(wr(&dev->adapter, 0x50, 0, buf, 1, rd, 3), 2);
		T_EQ(rd[0], 1);
		T_EQ(rd[2], 3);
		dw_del(dev);
		dwm_destroy(m);
	}
	/* not a DesignWare part */
	{
		struct dw_model *m = model_std(8);
		struct dw_i2c_dev d;

		kmemset(&d, 0, sizeof(d));
		m->comp_type = 0x12345678;
		d.base = m->window;
		i2c_strlcpy(d.name, "t", sizeof(d.name));
		T_EQ(i2c_dw_init_regmap(&d), -ENODEV);
		dwm_destroy(m);
	}
}

/* ==========================================================================
 * Transfers
 * ========================================================================== */

static void t_basic(void)
{
	struct dw_model *m = model_std(32);
	struct dw_i2c_dev *dev = dw_new(m, 100000, 400000);
	struct i2c_adapter *a = &dev->adapter;
	uint8_t w[5] = { 0x10, 0xa1, 0xa2, 0xa3, 0xa4 }, r[8];
	struct i2c_msg msg[4];

	/* single write */
	dwm_log_clear(m);
	T_EQ(wr(a, 0x50, 0, w, 5, NULL, 0), 1);
	T_STR(bus_log(m), "S 50W w10 wa1 wa2 wa3 wa4 P");
	T_EQ(g_ee->mem[0x13], 0xa4);

	/* combined write-then-read: one RESTART, one STOP */
	dwm_log_clear(m);
	T_EQ(wr(a, 0x50, 0, w, 1, r, 4), 2);
	T_STR(bus_log(m), "S 50W w10 Sr 50R ra1 ra2 ra3 ra4 P");

	/* plain read continues at the pointer */
	dwm_log_clear(m);
	T_EQ(wr(a, 0x50, 0, NULL, 0, r, 2), 1);
	T_STR(bus_log(m), "S 50R r00 r00 P");

	/* four messages: RESTART before each further one, even in the same
	 * direction */
	g_ee->mem[0x20] = 1;
	g_ee->mem[0x21] = 2;
	g_ee->mem[0x22] = 3;
	msg[0] = (struct i2c_msg){ 0x50, 0, 1, w };
	w[0] = 0x20;
	msg[1] = (struct i2c_msg){ 0x50, I2C_M_RD, 1, r };
	msg[2] = (struct i2c_msg){ 0x50, I2C_M_RD, 2, r + 1 };
	msg[3] = (struct i2c_msg){ 0x50, 0, 2, w + 1 };
	dwm_log_clear(m);
	T_EQ(i2c_transfer(a, msg, 4), 4);
	T_STR(bus_log(m),
	      "S 50W w20 Sr 50R r01 Sr 50R r02 r03 Sr 50W wa1 wa2 P");
	T_EQ(r[0], 1);
	T_EQ(r[2], 3);

	/* same direction, the second message starting on a full FIFO:
	 * still a RESTART between them (32-deep FIFO, 32 + 2 bytes) */
	{
		static uint8_t big[33];

		big[0] = 0x60; /* clear of the registers used below */
		for (int i = 1; i < 33; i++)
			big[i] = (uint8_t)(0x40 + i);
		msg[0] = (struct i2c_msg){ 0x50, 0, 32, big };
		msg[1] = (struct i2c_msg){ 0x50, 0, 2, w + 1 };
		dwm_log_clear(m);
		T_EQ(i2c_transfer(a, msg, 2), 2);
		T_ASSERT(str_has(bus_log(m), "w5f Sr 50W wa1 wa2 P"));
		/* and two reads, the first filling the FIFO */
		msg[0] = (struct i2c_msg){ 0x50, I2C_M_RD, 32, big };
		msg[1] = (struct i2c_msg){ 0x50, I2C_M_RD, 2, r };
		dwm_log_clear(m);
		T_EQ(i2c_transfer(a, msg, 2), 2);
		T_ASSERT(str_has(bus_log(m), " Sr 50R "));
		T_ASSERT(str_has(g_log, "S 50R "));
	}

	/* I2C_M_STOP splits into two transactions */
	msg[0] = (struct i2c_msg){ 0x50, I2C_M_STOP, 1, w };
	msg[1] = (struct i2c_msg){ 0x50, I2C_M_RD, 1, r };
	dwm_log_clear(m);
	T_EQ(i2c_transfer(a, msg, 2), 2);
	T_STR(bus_log(m), "S 50W w20 P S 50R r01 P");

	/* messages to two targets in one transfer: refused */
	msg[0] = (struct i2c_msg){ 0x50, 0, 1, w };
	msg[1] = (struct i2c_msg){ 0x15, I2C_M_RD, 1, r };
	T_EQ(i2c_transfer(a, msg, 2), -EINVAL);

	/* the master helpers */
	{
		struct i2c_client c;

		kmemset(&c, 0, sizeof(c));
		c.adapter = a;
		c.addr = 0x50;
		w[0] = 0x10;
		T_EQ(i2c_master_send(&c, (char *)w, 1), 1);
		T_EQ(i2c_master_recv(&c, (char *)r, 3), 3);
		T_EQ(r[0], 0xa1);
		T_EQ(i2c_master_send(&c, (char *)w, 70000), -EINVAL);
	}
	T_EQ(m->rx_over, 0);
	T_EQ(m->tx_over, 0);
	T_EQ(m->rx_under, 0);
	dw_del(dev);
	dwm_destroy(m);
}

static void t_hid_and_tenbit(void)
{
	struct dw_model *m = model_std(64);
	struct dw_i2c_dev *dev = dw_new(m, 120000, 400000);
	struct i2c_adapter *a = &dev->adapter;
	uint8_t reg[2] = { 0x01, 0x00 }, desc[30], w[3] = { 7, 0x55, 0x66 };
	uint8_t r[2];

	/* the descriptor read of an HID-over-I2C device */
	dwm_log_clear(m);
	T_EQ(wr(a, 0x15, 0, reg, 2, desc, 30), 2);
	T_EQ(kmemcmp(desc, g_hid_desc, 30), 0);
	T_ASSERT(str_has(bus_log(m), "S 15W w01 w00 Sr 15R r1e r00 r00 r01"));
	T_ASSERT(str_has(g_log, "r00 P"));

	/* 10-bit addressing */
	dwm_log_clear(m);
	T_EQ(wr(a, 0x2a5, I2C_M_TEN, w, 3, NULL, 0), 1);
	T_STR(bus_log(m), "S 2a5tW w07 w55 w66 P");
	T_EQ(wr(a, 0x2a5, I2C_M_TEN, w, 1, r, 2), 2);
	T_EQ(r[0], 0x55);
	T_EQ(r[1], 0x66);
	/* the 10-bit device is not there at 7 bits, and 0x2a5 is no 7-bit
	 * address */
	T_EQ(wr(a, 0x25, 0, w, 1, NULL, 0), -EREMOTEIO);
	T_EQ(wr(a, 0x2a5, 0, w, 1, NULL, 0), -EINVAL);
	T_EQ(wr(a, 0x400, I2C_M_TEN, w, 1, NULL, 0), -EINVAL);
	dw_del(dev);
	dwm_destroy(m);
}

/* Messages far longer than the FIFOs: refill and drain, never an
 * overrun. */
static void t_long(void)
{
	static const unsigned int depths[] = { 2, 8, 16, 64 };

	for (unsigned int d = 0; d < 4; d++) {
		struct dw_model *m = model_std(depths[d]);
		struct dw_i2c_dev *dev = dw_new(m, 100000, 1000000);
		uint8_t *w = kcalloc(1, 402), *r = kcalloc(1, 400);
		uint8_t reg[2] = { 0x40, 0x00 };

		w[0] = 0x40;
		w[1] = 0x00;
		for (int i = 0; i < 400; i++)
			w[i + 2] = (uint8_t)(i * 7 + d);
		T_EQ(wr(&dev->adapter, 0x15, 0, w, 402, NULL, 0), 1);
		T_EQ(wr(&dev->adapter, 0x15, 0, reg, 2, r, 400), 2);
		for (int i = 0; i < 400; i++)
			T_EQ(r[i], (uint8_t)(i * 7 + d));
		T_EQ(m->rx_over, 0);
		T_EQ(m->tx_over, 0);
		T_EQ(m->rx_under, 0);
		T_ASSERT(m->max_txflr <= depths[d]);
		kfree(w);
		kfree(r);
		dw_del(dev);
		dwm_destroy(m);
	}
}

static void t_smbus(void)
{
	struct dw_model *m = model_std(16);
	struct dw_i2c_dev *dev = dw_new(m, 100000, 100000);
	struct i2c_client c;
	uint8_t vals[32], blk[5] = { 9, 8, 7, 6, 5 };
	int w0;

	kmemset(&c, 0, sizeof(c));
	c.adapter = &dev->adapter;
	c.addr = 0x60;

	/* byte data, word data */
	T_EQ(i2c_smbus_write_byte_data(&c, 0x30, 0x5a), 0);
	T_EQ(i2c_smbus_read_byte_data(&c, 0x30), 0x5a);
	T_EQ(i2c_smbus_read_word_data(&c, 0x20), 0x1234);
	T_EQ(i2c_smbus_read_word_swapped(&c, 0x20), 0x3412);

	/* block read: the length byte from the device (I2C_M_RECV_LEN) */
	kmemcpy(g_smb->block, "\x11\x22\x33\x44\x55", 5);
	g_smb->block_len = 5;
	dwm_log_clear(m);
	T_EQ(i2c_smbus_read_block_data(&c, 0x10, vals), 5);
	T_EQ(vals[0], 0x11);
	T_EQ(vals[4], 0x55);
	T_STR(bus_log(m), "S 60W w10 Sr 60R r05 r11 r22 r33 r44 r55 P");

	/* an invalid length from the device still ends the transaction
	 * (one more byte), with a one-byte block */
	g_smb->bad_count = 40;
	T_EQ(i2c_smbus_read_block_data(&c, 0x10, vals), 1);
	g_smb->bad_count = 0;

	/* with packet error checking */
	c.flags = I2C_CLIENT_PEC;
	g_smb->pec = 1;
	T_EQ(i2c_smbus_read_block_data(&c, 0x10, vals), 5);
	T_EQ(vals[3], 0x44);
	T_EQ(i2c_smbus_read_word_data(&c, 0x20), 0x1234);
	T_EQ(i2c_smbus_read_byte_data(&c, 0x30), 0x5a);
	/* a write carries a PEC the device can check: the CRC over the
	 * whole transaction including it comes out 0 */
	T_EQ(i2c_smbus_write_block_data(&c, 0x11, 5, blk), 0);
	T_EQ(g_smb->crc_at_stop, 0);
	T_EQ(g_smb->block[4], 5);
	/* a corrupted PEC */
	g_smb->corrupt_pec = 1;
	T_EQ(i2c_smbus_read_block_data(&c, 0x10, vals), -EBADMSG);
	g_smb->corrupt_pec = 0;
	/* a shorter block than announced is consistent with its PEC */
	g_smb->bad_count = 4;
	T_EQ(i2c_smbus_read_block_data(&c, 0x10, vals), 4);
	g_smb->bad_count = 0;
	c.flags = 0;
	g_smb->pec = 0;

	/* I2C block */
	g_ee->mem[0x80] = 0xde;
	g_ee->mem[0x81] = 0xad;
	c.addr = 0x50;
	T_EQ(i2c_smbus_read_i2c_block_data(&c, 0x80, 2, vals), 2);
	T_EQ(vals[1], 0xad);
	T_EQ(i2c_smbus_write_i2c_block_data(&c, 0x90, 3, blk), 0);
	T_EQ(g_ee->mem[0x92], 7);
	T_EQ(i2c_smbus_read_i2c_block_data_or_emulated(&c, 0x90, 3, vals), 3);
	T_EQ(vals[2], 7);
	/* send/receive byte */
	T_EQ(i2c_smbus_write_byte(&c, 0x90), 0);
	T_EQ(i2c_smbus_read_byte(&c), 9);

	/* quick: zero length -- the controller cannot */
	{
		union i2c_smbus_data d;

		T_EQ(i2c_smbus_xfer(c.adapter, 0x50, 0, I2C_SMBUS_WRITE, 0,
				    I2C_SMBUS_QUICK, NULL),
		     -EOPNOTSUPP);
		d.block[0] = 33;
		T_EQ(i2c_smbus_xfer(c.adapter, 0x50, 0, I2C_SMBUS_WRITE, 0,
				    I2C_SMBUS_BLOCK_DATA, &d),
		     -EINVAL);
		T_EQ(i2c_smbus_xfer(c.adapter, 0x50, 0, I2C_SMBUS_READ, 0, 99,
				    &d),
		     -EOPNOTSUPP);
		w0 = g_test_warnings;
		T_EQ(i2c_smbus_xfer(c.adapter, 0x50, 0, I2C_SMBUS_READ, 0,
				    I2C_SMBUS_BYTE_DATA, NULL),
		     -EINVAL);
		EXPECT_WARNS(w0, 0);
	}
	T_EQ(dev->adapter.stats.rejected_quirk, 1);
	dw_del(dev);
	dwm_destroy(m);
}

static void t_errors(void)
{
	struct dw_model *m = model_std(16);
	struct dw_i2c_dev *dev = dw_new(m, 100000, 400000);
	struct i2c_adapter *a = &dev->adapter;
	uint8_t w[4] = { 1, 2, 3, 4 }, r[2];
	struct i2c_msg z = { 0x50, 0, 0, w };

	/* nobody at the address */
	dwm_log_clear(m);
	T_EQ(wr(a, 0x33, 0, w, 2, NULL, 0), -EREMOTEIO);
	T_STR(bus_log(m), "S 33W NA A0 P");
	T_EQ(dev->st.abort_src[ABRT_7B_ADDR_NOACK], 1);
	/* 10-bit: first address byte */
	T_EQ(wr(a, 0x111, I2C_M_TEN, w, 1, r, 1), -EREMOTEIO);
	T_EQ(dev->st.abort_src[ABRT_10ADDR1_NOACK], 1);
	/* a data byte not acknowledged */
	dwm_log_clear(m);
	T_EQ(wr(a, 0x34, 0, w, 4, NULL, 0), -EREMOTEIO);
	T_STR(bus_log(m), "S 34W w01 w02 ND A3 P");
	T_EQ(a->stats.err_nack, 3);

	/* zero-length messages: adapter quirk */
	T_EQ(i2c_transfer(a, &z, 1), -EOPNOTSUPP);
	z.flags = I2C_M_RD;
	T_EQ(i2c_transfer(a, &z, 1), -EOPNOTSUPP);

	/* lost arbitration: retried by the core, then success */
	m->arb_lose = 2;
	dwm_log_clear(m);
	T_EQ(wr(a, 0x50, 0, w, 2, NULL, 0), 1);
	T_STR(bus_log(m), "S AL A12 S AL A12 S 50W w01 w02 P");
	T_EQ(a->stats.retries, 2);
	/* more losses than retries: -EAGAIN */
	m->arb_lose = 10;
	T_EQ(wr(a, 0x50, 0, w, 2, NULL, 0), -EAGAIN);
	T_EQ(m->arb_lose, 6); /* 1 + 3 retries */
	m->arb_lose = 0;
	T_EQ(a->stats.err_arb, 1);

	/* the bus still works */
	T_EQ(wr(a, 0x50, 0, w, 1, r, 2), 2);
	T_EQ(r[0], 2);
	dw_del(dev);
	dwm_destroy(m);
}

static void t_timeout(void)
{
	struct dw_model *m = model_std(16);
	struct dw_i2c_dev *dev = dw_new(m, 100000, 400000);
	struct i2c_adapter *a = &dev->adapter;
	uint8_t w[3] = { 0x10, 1, 2 }, r[2];
	uint64_t t0;

	/* the target stretches the clock for 150 ms; the transfer may take
	 * 40 ms */
	g_slow->stretch_us = 150000;
	a->timeout = 40;
	t0 = h_now_ns();
	T_EQ(wr(a, 0x40, 0, w, 3, NULL, 0), -ETIMEDOUT);
	T_ASSERT(h_now_ns() - t0 < 140ull * 1000000ull);
	T_EQ(dev->st.timeouts, 1);
	T_EQ(a->stats.err_timeout, 1);
	/* the controller was re-initialised: disabled, programmed */
	T_EQ(m->enable & 1, 0);
	T_EQ(m->fs_hcnt, 87);
	g_slow->stretch_us = 0;
	a->timeout = 1000;
	/* and the bus works afterwards */
	T_EQ(wr(a, 0x50, 0, w, 1, r, 2), 2);
	dw_del(dev);
	dwm_destroy(m);
}

/* ==========================================================================
 * Interrupts
 * ========================================================================== */

static volatile int g_other_pending, g_other_handled;

static int other_isr(void *arg)
{
	(void)arg;
	if (!g_other_pending)
		return IRQ_NONE;
	g_other_pending = 0;
	mock_irq_set_ext(*(int *)arg, 0);
	g_other_handled++;
	return IRQ_HANDLED;
}

static void t_irq_sharing(void)
{
	struct dw_model *m = model_std(16);
	struct dw_i2c_dev *dev = dw_new(m, 100000, 400000);
	int irq = dev->irq;
	unsigned long none0 = dev->st.irq_none;
	uint8_t w[2] = { 0x10, 0x77 }, r[1];

	/* idle: not ours */
	T_EQ(i2c_dw_isr(dev), IRQ_NONE);
	T_EQ(dev->st.irq_none, none0 + 1);

	/* a second device on the line raises it: its handler claims it,
	 * ours says no */
	T_EQ(request_irq(irq, other_isr, IRQF_SHARED, "other", &irq), 0);
	g_other_pending = 1;
	mock_irq_set_ext(irq, 1);
	for (int i = 0; i < 200 && g_other_pending; i++)
		wait_ms(1);
	T_EQ(g_other_handled, 1);
	T_ASSERT(dev->st.irq_none > none0 + 1);
	/* transfers keep working with the sharer present */
	T_EQ(wr(&dev->adapter, 0x50, 0, w, 2, NULL, 0), 1);
	T_EQ(wr(&dev->adapter, 0x50, 0, w, 1, r, 1), 2);
	T_EQ(r[0], 0x77);
	(void)free_irq(irq, &irq);

	/* the controller vanished: all-ones, not ours */
	m->gone = 1;
	T_EQ(i2c_dw_isr(dev), IRQ_NONE);
	m->gone = 0;

	/* an interrupt with no transfer: acknowledged, masked */
	{
		uint64_t fl = local_irq_save();

		m->enable = 1;
		m->raw_latched = 1u << 9; /* a STOP_DET left by firmware */
		m->intr_mask = 1u << 9;
		T_EQ(i2c_dw_isr(dev), IRQ_HANDLED);
		T_EQ(m->intr_mask, 0);
		T_EQ(dev->st.irq_unexpected, 1);
		m->enable = 0;
		m->raw_latched = 0;
		local_irq_restore(fl);
	}
	dw_del(dev);
	dwm_destroy(m);
}

/* ==========================================================================
 * Concurrency and power
 * ========================================================================== */

struct worker {
	struct i2c_adapter *a;
	int id, iters, errors;
	volatile int done;
};

static void worker_fn(void *arg)
{
	struct worker *w = arg;

	for (int i = 0; i < w->iters; i++) {
		uint8_t buf[9], r[8];

		buf[0] = (uint8_t)(w->id * 32);
		for (int k = 0; k < 8; k++)
			buf[k + 1] = (uint8_t)(w->id * 50 + i + k);
		if (wr(w->a, 0x50, 0, buf, 9, NULL, 0) != 1)
			w->errors++;
		if (wr(w->a, 0x50, 0, buf, 1, r, 8) != 2)
			w->errors++;
		else if (kmemcmp(r, buf + 1, 8))
			w->errors++;
	}
	__atomic_store_n(&w->done, 1, __ATOMIC_RELEASE);
}

static void t_concurrent(void)
{
	struct dw_model *m = model_std(8);
	struct dw_i2c_dev *dev = dw_new(m, 100000, 1000000);
	struct worker w[4];

	for (int i = 0; i < 4; i++) {
		w[i] = (struct worker){ &dev->adapter, i, 100, 0, 0 };
		T_ASSERT(sched_add_task(worker_fn, &w[i], NULL, 0));
	}
	for (int i = 0; i < 4; i++) {
		while (!__atomic_load_n(&w[i].done, __ATOMIC_ACQUIRE))
			wait_ms(2);
		T_EQ(w[i].errors, 0);
	}
	/* every transaction on the wire was one thread's: a STOP for
	 * each START, nothing aborted */
	T_EQ(m->starts, m->stops);
	T_EQ(m->aborts, 0);
	T_EQ(m->starts, 800);
	dw_del(dev);
	dwm_destroy(m);
}

struct loop_ctx {
	struct i2c_adapter *a;
	volatile int stop, ok, shut, other, done;
};

static void loop_fn(void *arg)
{
	struct loop_ctx *c = arg;
	uint8_t w[2] = { 0x10, 0x42 }, r[1];

	while (!c->stop) {
		int ret = wr(c->a, 0x50, 0, w, 1, r, 1);

		if (ret == 2)
			c->ok++;
		else if (ret == -ESHUTDOWN)
			c->shut++;
		else
			c->other++;
	}
	__atomic_store_n(&c->done, 1, __ATOMIC_RELEASE);
}

static void t_suspend_race(void)
{
	struct dw_model *m = model_std(8);
	struct dw_i2c_dev *dev = dw_new(m, 100000, 400000);
	struct loop_ctx c = { &dev->adapter, 0, 0, 0, 0, 0 };
	int w0 = g_test_warnings, ok_before;

	T_ASSERT(sched_add_task(loop_fn, &c, NULL, 0));
	while (c.ok < 20)
		wait_ms(1);
	/* system suspend of the controller in the middle of transfers */
	T_EQ(i2c_dw_suspend(dev), 0);
	T_EQ(m->enable & 1, 0);
	T_ASSERT(dev->rpm_suspended);
	/* A transfer that completed just before the suspend is counted by
	 * the loop a moment later: let that land before the snapshot. */
	wait_ms(5);
	ok_before = c.ok;
	wait_ms(30);
	T_ASSERT(c.shut > 0);
	T_ASSERT(c.ok == ok_before); /* nothing reached the hardware */
	T_EQ(c.other, 0);
	EXPECT_WARNS(w0, 1); /* "transfer while suspended", once */
	T_EQ(i2c_dw_resume(dev), 0);
	ok_before = c.ok;
	wait_ms(30);
	T_ASSERT(c.ok > ok_before);
	c.stop = 1;
	while (!c.done)
		wait_ms(1);
	T_EQ(c.other, 0);
	/* the second suspend warns again (once) */
	w0 = g_test_warnings;
	i2c_mark_adapter_suspended(&dev->adapter);
	T_EQ(wr(&dev->adapter, 0x50, 0, NULL, 0, (uint8_t *)&c, 1),
	     -ESHUTDOWN);
	T_EQ(wr(&dev->adapter, 0x50, 0, NULL, 0, (uint8_t *)&c, 1),
	     -ESHUTDOWN);
	EXPECT_WARNS(w0, 1);
	i2c_mark_adapter_resumed(&dev->adapter);
	dw_del(dev);
	dwm_destroy(m);
}

static void t_runtime_pm(void)
{
	struct dw_model *m = model_std(16);
	struct dw_i2c_dev *dev;
	uint8_t w[2] = { 0x10, 0x99 }, r[1];

	m->lpss = 1;
	m->lpss_caps = 0;
	dev = dw_new(m, 120000, 400000);
	T_EQ(m->priv[1], 7); /* RESETS: function and DMA out of reset */
	T_EQ(m->priv[0x40 / 4], (uint32_t)(unsigned long)m->window);
	i2c_dw_rpm_enable(dev, 20);
	T_EQ(wr(&dev->adapter, 0x50, 0, w, 2, NULL, 0), 1);
	/* idle for longer than the delay: suspended, LPSS in reset */
	for (int i = 0; i < 200 && !dev->rpm_suspended; i++)
		wait_ms(1);
	T_ASSERT(dev->rpm_suspended);
	T_EQ(m->priv[1], 0);
	T_EQ(m->enable & 1, 0);
	/* interrupts are never ours while suspended */
	T_EQ(i2c_dw_isr(dev), IRQ_NONE);
	/* the next transfer brings it back */
	T_EQ(wr(&dev->adapter, 0x50, 0, w, 1, r, 1), 2);
	T_EQ(r[0], 0x99);
	T_EQ(m->priv[1], 7);
	T_EQ(m->fs_hcnt, 105);
	T_ASSERT(dev->st.rpm_resumes >= 1);
	/* busy transfers keep it up */
	for (int i = 0; i < 20; i++) {
		T_EQ(wr(&dev->adapter, 0x50, 0, w, 2, NULL, 0), 1);
		wait_ms(5);
		T_ASSERT(!dev->rpm_suspended);
	}
	dw_del(dev);
	dwm_destroy(m);
}

static void t_atomic(void)
{
	struct dw_model *m = model_std(8);
	struct dw_i2c_dev *dev = dw_new(m, 100000, 400000);
	uint8_t w[3] = { 0x30, 0xc1, 0xc2 }, r[2];
	uint64_t fl;
	int ret;

	/* interrupts off: the polled path, no sleeping (the prelude would
	 * abort on a sleep) */
	fl = local_irq_save();
	T_ASSERT(i2c_in_atomic_xfer_mode());
	ret = wr(&dev->adapter, 0x50, 0, w, 3, NULL, 0);
	local_irq_restore(fl);
	T_EQ(ret, 1);
	fl = local_irq_save();
	ret = wr(&dev->adapter, 0x50, 0, w, 1, r, 2);
	local_irq_restore(fl);
	T_EQ(ret, 2);
	T_EQ(r[1], 0xc2);
	T_ASSERT(dev->st.polled >= 2);
	/* a busy bus lock: the atomic path does not wait */
	i2c_lock_bus(&dev->adapter, I2C_LOCK_SEGMENT);
	{
		struct loop_ctx *dummy = NULL;

		(void)dummy;
	}
	i2c_unlock_bus(&dev->adapter, I2C_LOCK_SEGMENT);

	/* a controller without an interrupt: polled throughout */
	{
		struct dw_model *m2 = model_std(8);
		struct dw_i2c_dev *d2 = dw_new_ex(m2, 100000, 400000, 1, NULL);

		T_ASSERT(d2->flags & ACCESS_POLLING);
		T_EQ(wr(&d2->adapter, 0x15, 0, (const uint8_t *)"\x01\x00", 2,
			r, 2),
		     2);
		T_EQ(r[0], 0x1e);
		T_EQ(m2->intr_mask, 0);
		dw_del(d2);
		dwm_destroy(m2);
	}
	dw_del(dev);
	dwm_destroy(m);
}

/* ==========================================================================
 * Recovery
 * ========================================================================== */

static void t_recovery_busclear(void)
{
	struct dw_model *m = model_std(8);
	struct dw_i2c_dev *dev;
	uint8_t w[2] = { 0x10, 0x31 }, r[1];

	m->version = 0x3230322Au; /* 2.02a */
	m->bus_clear_hw = 1;
	m->con |= 1u << 11; /* firmware turned on the bus clear */
	dev = dw_new(m, 100000, 400000);
	T_ASSERT(dev->hw_bus_clear);
	T_ASSERT(dev->master_cfg & DW_IC_CON_BUS_CLEAR_CTRL);
	T_ASSERT(dev->adapter.bus_recovery_info);
	/* a target holds SDA: the bus never goes idle, the bus clear frees
	 * it, the transfer proceeds */
	m->stuck_sda = 1;
	m->stuck_clocks = 5;
	dwm_log_clear(m);
	T_EQ(wr(&dev->adapter, 0x50, 0, w, 2, NULL, 0), 1);
	T_ASSERT(str_has(bus_log(m), "REC"));
	T_EQ(dev->st.bus_busy, 1);
	T_EQ(dev->adapter.stats.recoveries, 1);
	/* one that cannot be freed */
	m->stuck_sda = 1;
	m->stuck_clocks = 50;
	T_EQ(wr(&dev->adapter, 0x50, 0, w, 1, r, 1), -ETIMEDOUT);
	T_EQ(dev->adapter.stats.recovery_failed, 1);
	m->stuck_sda = 0;
	m->sda_not_recovered = 0;
	T_EQ(wr(&dev->adapter, 0x50, 0, w, 1, r, 1), 2);
	T_EQ(r[0], 0x31);
	dw_del(dev);
	dwm_destroy(m);

	/* without the feature: no recovery, the busy bus is an error */
	m = model_std(8);
	dev = dw_new(m, 100000, 400000);
	T_ASSERT(!dev->adapter.bus_recovery_info);
	m->stuck_sda = 1;
	T_EQ(wr(&dev->adapter, 0x50, 0, w, 2, NULL, 0), -ETIMEDOUT);
	T_EQ(i2c_recover_bus(&dev->adapter), -EBUSY);
	m->stuck_sda = 0;
	dw_del(dev);
	dwm_destroy(m);
}

/* _DSD with "scl-gpios" and "sda-gpios" for the adapter's device */
static UINT8 g_props_uuid[16];
static ACPI_OBJECT g_dsd_top[2], g_dsd_props[2], g_dsd_kv[2][2],
	g_dsd_ref[2][4];
static struct mock_node *g_dsd_owner;

static ACPI_STATUS dsd_gpio_eval(struct mock_node *n,
				 const ACPI_OBJECT_LIST *a, ACPI_OBJECT *o)
{
	(void)a;
	(void)n;
	T_EQ(acpi_uuid_parse("daffd814-6eba-4d8c-8a91-bc9bbf4aa301",
			     g_props_uuid),
	     0);
	for (int i = 0; i < 2; i++) {
		mock_obj_ref(&g_dsd_ref[i][0], g_dsd_owner);
		mock_obj_int(&g_dsd_ref[i][1], (UINT64)i);
		mock_obj_int(&g_dsd_ref[i][2], 0);
		mock_obj_int(&g_dsd_ref[i][3], 0);
		mock_obj_str(&g_dsd_kv[i][0], i ? "sda-gpios" : "scl-gpios");
		mock_obj_pkg(&g_dsd_kv[i][1], g_dsd_ref[i], 4);
		mock_obj_pkg(&g_dsd_props[i], g_dsd_kv[i], 2);
	}
	mock_obj_buf(&g_dsd_top[0], g_props_uuid, 16);
	mock_obj_pkg(&g_dsd_top[1], g_dsd_props, 2);
	if (o)
		mock_obj_pkg(o, g_dsd_top, 2);
	return AE_OK;
}

static void t_recovery_gpio(void)
{
	struct dw_model *m = model_std(8);
	struct mock_node *ctl;
	struct acpi_device *adev;
	struct dw_i2c_dev *dev;
	uint8_t w[2] = { 0x10, 0x5c };

	mock_ns_reset();
	i2c_acpi_invalidate_cache();
	ctl = mock_device(NULL, "I2CX", "TEST0I2C", NULL);
	g_dsd_owner = ctl;
	(void)mock_method(ctl, "_DSD", dsd_gpio_eval, NULL);
	adev = acpi_dev_from_handle(ctl);
	T_ASSERT(adev);
	g_mock_recovery_model = m;
	dev = dw_new_ex(m, 100000, 400000, 0, adev);
	T_ASSERT(dev->adapter.bus_recovery_info);
	T_ASSERT(dev->rinfo.scl_gpiod && dev->rinfo.sda_gpiod);
	T_ASSERT(dev->adapter.bus_recovery_info->get_sda);
	/* SDA held for four clocks: nine pulses at most free it */
	m->stuck_sda = 1;
	m->stuck_clocks = 4;
	T_EQ(i2c_recover_bus(&dev->adapter), 0);
	T_EQ(m->stuck_sda, 0);
	/* and inside a transfer */
	m->stuck_sda = 1;
	m->stuck_clocks = 3;
	T_EQ(wr(&dev->adapter, 0x50, 0, w, 2, NULL, 0), 1);
	T_EQ(g_ee->mem[0x10], 0x5c);
	/* SCL held low by somebody: recovery gives up */
	m->scl_gpio = 0;
	m->stuck_sda = 1;
	m->stuck_clocks = 100;
	{
		struct i2c_bus_recovery_info *bri =
			dev->adapter.bus_recovery_info;
		int (*get_scl)(struct i2c_adapter *) = bri->get_scl;

		(void)get_scl;
	}
	m->stuck_sda = 0;
	m->scl_gpio = 1;
	dw_del(dev);
	g_mock_recovery_model = NULL;
	acpi_dev_put(adev);
	dwm_destroy(m);
	i2c_acpi_invalidate_cache();
}

/* ==========================================================================
 * ACPI: the HP ProBook 650 G8 and Dell Pro 16 Plus shapes over PCI
 * ========================================================================== */

static union rslot g_crs_a[8], g_crs_b[8], g_crs_c[8], g_crs_d[8];
static UINT16 g_pin_a[] = { 0x147 }, g_pin_b[] = { 0x10 };
static ACPI_OBJECT g_cn_e[4][3];

#define CN_EVAL(name, idx, h, l, s)                                       \
	static ACPI_STATUS name(struct mock_node *n,                      \
				const ACPI_OBJECT_LIST *a, ACPI_OBJECT *o) \
	{                                                                 \
		(void)n;                                                  \
		(void)a;                                                  \
		mock_obj_int(&g_cn_e[idx][0], h);                         \
		mock_obj_int(&g_cn_e[idx][1], l);                         \
		mock_obj_int(&g_cn_e[idx][2], s);                         \
		if (o)                                                    \
			mock_obj_pkg(o, g_cn_e[idx], 3);                  \
		return AE_OK;                                             \
	}
/* HP: SSCN/FMCN of I2C0; Dell: FPCN of I2C3 */
CN_EVAL(hp_sscn, 0, 0x264, 0x2c2, 0x1c)
CN_EVAL(hp_fmcn, 1, 0x62, 0xc5, 0x1c)
CN_EVAL(dell_fpcn, 2, 0x3c, 0x5e, 0x1a)

static ACPI_STATUS sta_zero(struct mock_node *n, const ACPI_OBJECT_LIST *a,
			    ACPI_OBJECT *o)
{
	(void)n;
	(void)a;
	if (o)
		mock_obj_int(o, 0);
	return AE_OK;
}

/* A test client driver of HID-over-I2C devices. */
static int g_probes, g_removes, g_probe_irq, g_probe_wake, g_probe_ret;
static int g_suspends, g_resumes;
static uint8_t g_probe_desc[30];
static struct i2c_client *g_probed;

static int tdrv_probe(struct i2c_client *client)
{
	uint8_t reg[2] = { 0x01, 0x00 };

	g_probes++;
	g_probed = client;
	g_probe_irq = client->irq;
	g_probe_wake = (client->flags & I2C_CLIENT_WAKE) != 0;
	if (g_probe_ret)
		return g_probe_ret;
	/* the descriptor read a transport does in its probe */
	if (wr(client->adapter, client->addr, client->flags, reg, 2,
	       g_probe_desc, 30) != 2)
		return -EIO;
	return 0;
}

static void tdrv_remove(struct i2c_client *client)
{
	(void)client;
	g_removes++;
}

static int tdrv_suspend(struct i2c_client *client)
{
	(void)client;
	g_suspends++;
	return 0;
}

static int tdrv_resume(struct i2c_client *client)
{
	(void)client;
	g_resumes++;
	return 0;
}

static const struct acpi_device_id tdrv_acpi_ids[] = {
	{ "PNP0C50", 0x50 },
	{ "ACPI0C50", 0x51 },
	{ "", 0 },
};

static struct i2c_driver g_tdrv = {
	.name = "test_i2c_hid",
	.probe = tdrv_probe,
	.remove = tdrv_remove,
	.suspend = tdrv_suspend,
	.resume = tdrv_resume,
	.acpi_match_table = tdrv_acpi_ids,
};

static void hp_namespace(void)
{
	struct mock_node *pc00, *i2c0, *tpd0, *alt;
	int i = 0;

	mock_ns_reset();
	i2c_acpi_invalidate_cache();
	pc00 = mock_device(NULL, "PC00", "PNP0A08", NULL);
	mock_add_cid(pc00, "PNP0A03");
	i2c0 = mock_device(pc00, "I2C0", NULL, NULL);
	mock_set_adr(i2c0, 0x00150000);
	(void)mock_method(i2c0, "SSCN", hp_sscn, NULL);
	(void)mock_method(i2c0, "FMCN", hp_fmcn, NULL);
	/* an alternative touchpad of the same model line, not present */
	alt = mock_device(i2c0, "TPD1", "SYNA7DB5", NULL);
	mock_add_cid(alt, "PNP0C50");
	(void)mock_method(alt, "_STA", sta_zero, NULL);
	tpd0 = mock_device(i2c0, "TPD0", "ELAN073B", "1");
	mock_add_cid(tpd0, "PNP0C50");
	mock_res_i2c(&g_crs_a[i++].r, 0x15, 400000, 0, "\\_SB.PC00.I2C0", 1);
	mock_res_gpio(&g_crs_a[i++].r, 1, 1, ACPI_ACTIVE_LOW, 0, 1,
		      ACPI_PIN_CONFIG_PULLUP, 0, 0, "\\_SB.GPI0", g_pin_a, 1);
	mock_res_end(&g_crs_a[i++].r);
	(void)mock_resources_stride(tpd0, "_CRS", g_crs_a, i,
				    (int)sizeof(union rslot));
	i = 0;
	mock_res_i2c(&g_crs_b[i++].r, 0x2c, 400000, 0, "\\_SB.PC00.I2C0", 1);
	mock_res_end(&g_crs_b[i++].r);
	(void)mock_resources_stride(alt, "_CRS", g_crs_b, i,
				    (int)sizeof(union rslot));
	(void)mock_device(NULL, "GPI0", "INT34C5", NULL);
}

static void t_hp_probook(void)
{
	struct dw_model *m = model_std(64);
	struct mock_pci mp;
	struct i2c_adapter *a;
	struct i2c_client *c;
	struct dw_i2c_dev *dev;
	char *buf;
	long n;

	m->lpss = 1;
	m->lpss_caps = 0x200; /* I2C, iDMA present */
	m->priv[0xfc / 4] = m->lpss_caps;
	hp_namespace();
	kmemset(&mp, 0, sizeof(mp));
	mp.vendor = 0x8086;
	mp.device_id = 0xa0e8; /* Tiger Lake-LP I2C0 */
	mp.slot = 0x15;
	mp.m = m;
	mp.bar = (uint64_t)(unsigned long)m->window;
	mock_pci_reset();
	mock_pci_add(&mp);
	kmemset(&g_mock_gpioint[0], 0, sizeof(g_mock_gpioint[0]));
	i2c_strlcpy(g_mock_gpioint[0].hid, "ELAN073B",
		    sizeof(g_mock_gpioint[0].hid));
	g_mock_gpioint[0].defer = 1; /* the GPIO controller is not up yet */
	g_mock_gpioint[0].wake = 1;
	g_probes = g_removes = 0;

	T_EQ(i2c_register_driver(&g_tdrv), 0);
	T_EQ(i2c_dw_pci_init(), 0);
	T_EQ(mp.probe_ret, 0);
	T_EQ(mp.cmd & 0x6, 0x6); /* memory decoding and bus mastering */
	T_EQ(m->priv[1], 7);
	T_EQ(m->priv[0x40 / 4], (uint32_t)(unsigned long)m->window);
	T_EQ(m->priv[0x44 / 4], (uint32_t)((unsigned long)m->window >> 32));

	a = i2c_find_adapter_by_acpi_handle(mock_lookup("\\_SB.PC00.I2C0"));
	T_ASSERT(a);
	dev = i2c_get_adapdata(a);
	T_STR(a->name, "Synopsys DesignWare I2C adapter");
	T_STR(dev->name, "i2c_designware 0000:00:15.0");
	T_EQ(a->bus_freq_hz, 400000);
	T_EQ(dev->clk_khz, 120000);
	/* FMCN counts and hold (+ RX hold) */
	T_EQ(m->fs_hcnt, 0x62);
	T_EQ(m->fs_lcnt, 0xc5);
	T_EQ(m->ss_hcnt, 0x264);
	T_EQ(m->sda_hold, 0x1001c);
	T_EQ((m->con >> 1) & 3, 2);

	/* the client, named after its ACPI device, deferred on its GPIO
	 * interrupt */
	c = i2c_find_client(a, 0x15);
	T_ASSERT(c);
	T_STR(c->name, "ELAN073B:00");
	T_STR(c->dev_name, "i2c-ELAN073B:00");
	T_ASSERT(c->acpi_dev && str_eq(c->acpi_dev->hid, "ELAN073B"));
	T_EQ(g_probes, 0);
	T_ASSERT(c->deferred);
	T_ASSERT(g_mock_gpioint[0].calls >= 1);
	/* the not-present alternative got no client */
	T_ASSERT(!i2c_find_client(a, 0x2c));
	i2c_client_put(c);

	/* the GPIO controller arrives */
	g_mock_gpioint[0].defer = 0;
	i2c_retry_deferred_probes();
	T_EQ(g_probes, 1);
	T_EQ(g_probe_irq, g_mock_gpioint[0].irq);
	T_ASSERT(g_probe_irq > 0);
	T_EQ(g_probe_wake, 1);
	T_EQ(kmemcmp(g_probe_desc, g_hid_desc, 30), 0);
	T_EQ(i2c_get_match_data(g_probed), 0x50);

	/* /proc/i2c and /proc/bus/i2c */
	buf = kcalloc(1, 8192);
	n = i2c_core_show(buf, 8192);
	T_ASSERT(n > 0 && n < 8192);
	T_ASSERT(str_has(buf, "Synopsys DesignWare I2C adapter"));
	T_ASSERT(str_has(buf, "client 0x15 ELAN073B:00 [i2c-ELAN073B:00]"));
	T_ASSERT(str_has(buf, "driver test_i2c_hid"));
	T_ASSERT(str_has(buf, "dw: v1.21a"));
	n = i2c_proc_bus_show(NULL, buf, 8192);
	T_ASSERT(str_has(buf, "\ti2c\tSynopsys DesignWare I2C adapter\tI2C adapter\n"));
	kfree(buf);

	/* system suspend/resume through the core */
	g_suspends = g_resumes = 0;
	T_EQ(i2c_pm_suspend_all(), 0);
	T_EQ(g_suspends, 1);
	T_ASSERT(irq_is_wake_enabled(g_probe_irq));
	T_ASSERT(i2c_adapter_is_suspended(a));
	T_EQ(i2c_pm_resume_all(), 0);
	T_EQ(g_resumes, 1);
	T_ASSERT(!irq_is_wake_enabled(g_probe_irq));
	T_ASSERT(!i2c_adapter_is_suspended(a));

	i2c_put_adapter(a);
	i2c_del_driver(&g_tdrv);
	T_EQ(g_removes, 1);
	i2c_dw_pci_exit();
	T_EQ(mp.mapped, 0);
	T_EQ(mp.irq, 0);
	T_ASSERT(!i2c_find_adapter_by_acpi_handle(mock_lookup("\\_SB.PC00.I2C0")));
	dwm_destroy(m);
	mock_pci_reset();
	kmemset(&g_mock_gpioint[0], 0, sizeof(g_mock_gpioint[0]));
	i2c_acpi_invalidate_cache();
}

static void dell_namespace(void)
{
	struct mock_node *pc00, *i2c0, *i2c3, *tpd, *other, *dup;
	int i = 0;

	mock_ns_reset();
	i2c_acpi_invalidate_cache();
	pc00 = mock_device(NULL, "PC00", "PNP0A08", NULL);
	i2c0 = mock_device(pc00, "I2C0", NULL, NULL);
	mock_set_adr(i2c0, 0x00150000);
	i2c3 = mock_device(pc00, "I2C3", NULL, NULL);
	mock_set_adr(i2c3, 0x00150003);
	(void)mock_method(i2c3, "FPCN", dell_fpcn, NULL);
	/* an unrelated device on I2C0 */
	other = mock_device(i2c0, "SNSR", "TEST0050", NULL);
	mock_res_i2c(&g_crs_c[i++].r, 0x50, 400000, 0, "\\_SB.PC00.I2C0", 1);
	mock_res_end(&g_crs_c[i++].r);
	(void)mock_resources_stride(other, "_CRS", g_crs_c, i,
				    (int)sizeof(union rslot));
	/* the touchpad: on I2C3 at 1 MHz, with an APIC interrupt in this
	 * variant; a not-present twin of the same id comes first */
	dup = mock_device(i2c0, "TPDX", "VEN_0488", NULL);
	(void)mock_method(dup, "_STA", sta_zero, NULL);
	tpd = mock_device(i2c3, "TPD0", "VEN_0488", "1");
	mock_add_cid(tpd, "PNP0C50");
	i = 0;
	mock_res_i2c(&g_crs_d[i++].r, 0x2c, 1000000, 0, "\\_SB.PC00.I2C3", 1);
	{
		static const UINT32 gsi[] = { 27 };

		mock_res_extirq(&g_crs_d[i++].r, 1, ACPI_ACTIVE_LOW, 0, 0, gsi,
				1);
	}
	mock_res_end(&g_crs_d[i++].r);
	(void)mock_resources_stride(tpd, "_CRS", g_crs_d, i,
				    (int)sizeof(union rslot));
	(void)g_pin_b;
}

static void t_dell_pro(void)
{
	struct dw_model *m0 = model_std(64), *m3 = model_std(64);
	struct dwm_slave *tp = dwm_slave_new(SLV_HID, 0x2c, 0);
	struct mock_pci p0, p3;
	struct i2c_adapter *a0, *a3;
	struct dw_i2c_dev *d0, *d3;
	struct i2c_client *c;
	int irqs0;

	kmemcpy(&tp->mem[1], g_hid_desc, 30);
	dwm_add_slave(m3, tp);
	m0->lpss = m3->lpss = 1;
	dell_namespace();
	kmemset(&p0, 0, sizeof(p0));
	kmemset(&p3, 0, sizeof(p3));
	p0 = (struct mock_pci){ .vendor = 0x8086, .device_id = 0x7778,
				.slot = 0x15, .func = 0, .m = m0,
				.bar = (uint64_t)(unsigned long)m0->window,
				.msi = 1 };
	p3 = (struct mock_pci){ .vendor = 0x8086, .device_id = 0x777b,
				.slot = 0x15, .func = 3, .m = m3,
				.bar = (uint64_t)(unsigned long)m3->window };
	mock_pci_reset();
	mock_pci_add(&p0);
	mock_pci_add(&p3);
	irqs0 = mock_irqs_in_use();
	g_probes = g_removes = 0;
	g_probe_ret = 0;

	T_EQ(i2c_dw_pci_init(), 0);
	a0 = i2c_find_adapter_by_acpi_handle(mock_lookup("\\_SB.PC00.I2C0"));
	a3 = i2c_find_adapter_by_acpi_handle(mock_lookup("\\_SB.PC00.I2C3"));
	T_ASSERT(a0 && a3);
	T_EQ(a3->nr, a0->nr + 1);
	d0 = i2c_get_adapdata(a0);
	d3 = i2c_get_adapdata(a3);
	T_STR(d3->name, "i2c_designware 0000:00:15.3");
	T_STR(d0->irq_kind, "MSI");
	T_STR(d3->irq_kind, "INTx");
	/* Arrow Lake-H: 133 MHz, the platform's fall/hold times */
	T_EQ(d0->clk_khz, 133000);
	T_EQ(a0->bus_freq_hz, 400000);
	T_EQ(m0->fs_hcnt, 100);
	T_EQ(m0->fs_lcnt, 200);
	T_EQ(m0->sda_hold, 0x10006); /* 133 MHz * 42 ns = 6 cycles */
	/* I2C3: 1 MHz, FPCN counts in the fast registers */
	T_EQ(a3->bus_freq_hz, 1000000);
	T_EQ(m3->fs_hcnt, 0x3c);
	T_EQ(m3->fs_lcnt, 0x5e);
	T_EQ(m3->sda_hold, 0x1001a);

	c = i2c_find_client(a3, 0x2c);
	T_ASSERT(c);
	/* the not-present twin earlier in the namespace took instance 0 */
	T_STR(c->name, "VEN_0488:01");
	T_STR(c->dev_name, "i2c-VEN_0488:01");
	i2c_client_put(c);
	c = i2c_find_client(a0, 0x50);
	T_ASSERT(c);
	T_STR(c->name, "TEST0050:00");
	i2c_client_put(c);

	/* the driver binds; the interrupt is the APIC line */
	mock_gsi_bind(27, NULL);
	T_EQ(i2c_register_driver(&g_tdrv), 0);
	T_EQ(g_probes, 1);
	T_ASSERT(g_probe_irq > 0);
	T_EQ(g_probe_wake, 0);
	T_EQ(kmemcmp(g_probe_desc, g_hid_desc, 30), 0);
	/* its probe ran at 1 MHz on I2C3 */
	T_ASSERT(tp->txns >= 1);
	i2c_del_driver(&g_tdrv);
	T_EQ(g_removes, 1);
	/* a failing probe leaves it unbound with its interrupt released */
	g_probe_ret = -ENODEV;
	T_EQ(i2c_register_driver(&g_tdrv), 0);
	T_EQ(g_probes, 2);
	c = i2c_find_client(a3, 0x2c);
	T_ASSERT(c && !c->driver && !c->deferred && c->irq == 0);
	i2c_client_put(c);
	i2c_del_driver(&g_tdrv);
	g_probe_ret = 0;

	i2c_put_adapter(a0);
	i2c_put_adapter(a3);
	i2c_dw_pci_exit();
	T_EQ(mock_irqs_in_use(), irqs0);
	dwm_destroy(m0);
	dwm_destroy(m3);
	mock_pci_reset();
	i2c_acpi_invalidate_cache();
}

/* Bus speeds from the connections, with the firmware fixups. */
static union rslot g_spd[6][4];

static void spd_dev(struct mock_node *parent, const char *name,
		    const char *hid, int k, uint16_t addr, uint32_t speed,
		    const char *src)
{
	struct mock_node *d = mock_device(parent, name, hid, NULL);

	mock_res_i2c(&g_spd[k][0].r, addr, speed, 0, src, 1);
	mock_res_end(&g_spd[k][1].r);
	(void)mock_resources_stride(d, "_CRS", g_spd[k], 2,
				    (int)sizeof(union rslot));
}

static void t_speed(void)
{
	struct mock_node *bus1, *bus2, *bus3, *bus4;

	mock_ns_reset();
	i2c_acpi_invalidate_cache();
	bus1 = mock_device(NULL, "BUS1", "TESTBUS1", NULL);
	bus2 = mock_device(NULL, "BUS2", "TESTBUS2", NULL);
	bus3 = mock_device(NULL, "BUS3", "TESTBUS3", NULL);
	bus4 = mock_device(NULL, "BUS4", "TESTBUS4", NULL);
	spd_dev(bus1, "DEVA", "TESTA001", 0, 0x10, 1000000, "\\_SB.BUS1");
	spd_dev(bus1, "DEVB", "TESTB001", 1, 0x11, 400000, "\\_SB.BUS1");
	/* a device elsewhere naming BUS2, and one forcing 100 kHz */
	spd_dev(NULL, "DEVC", "TESTC001", 2, 0x12, 1000000, "\\_SB.BUS2");
	spd_dev(bus3, "DEVD", "ELAN06FA", 3, 0x15, 400000, "\\_SB.BUS3");
	spd_dev(bus3, "DEVE", "MSSL1680", 4, 0x40, 100000, "\\_SB.BUS4");
	T_EQ(i2c_acpi_find_bus_speed(bus1), 400000);
	T_EQ(i2c_acpi_find_bus_speed(bus2), 1000000);
	T_EQ(i2c_acpi_find_bus_speed(bus3), 100000);
	T_EQ(i2c_acpi_find_bus_speed(bus4), 400000);
	T_EQ(i2c_acpi_find_bus_speed(mock_lookup("\\_SB.DEVA")), 0);
	/* a non-standard speed rounds down in the controller */
	{
		struct dw_model *m = model_std(8);
		struct mock_node *bus5 = mock_device(NULL, "BUS5", "TESTBUS5",
						     NULL);
		struct acpi_device *adev;
		struct dw_i2c_dev *dev;

		i2c_acpi_invalidate_cache();
		spd_dev(bus5, "DEVF", "TESTF001", 5, 0x20, 380000,
			"\\_SB.BUS5");
		adev = acpi_dev_from_handle(bus5);
		dev = dw_new_ex(m, 100000, 0, 0, adev);
		T_EQ(dev->timings.bus_freq_hz, 100000);
		T_EQ((m->con >> 1) & 3, 1);
		dw_del(dev);
		acpi_dev_put(adev);
		dwm_destroy(m);
	}
	i2c_acpi_invalidate_cache();
}

/* ==========================================================================
 * ACPI platform controllers (AMD FCH, Bay Trail)
 * ========================================================================== */

static union rslot g_plat_crs[2][4], g_plat_tp[4];

static void t_platform(void)
{
	struct dw_model *m = model_std(32), *mb = model_std(32);
	struct mock_node *amd, *byt, *tp;
	struct i2c_adapter *a, *ab;
	struct dw_i2c_dev *dev;
	struct i2c_client *c;
	static const UINT32 gsi[] = { 20 };
	uint8_t w[2] = { 0x10, 0x3e }, r[1];
	int i;

	mock_ns_reset();
	i2c_acpi_invalidate_cache();
	amd = mock_device(NULL, "I2CA", "AMDI0010", "0");
	i = 0;
	mock_res_addr64(&g_plat_crs[0][i++].r, (UINT64)(unsigned long)m->window,
			(UINT64)(unsigned long)m->window + 0xfff, 0x1000, 0, 0);
	mock_res_extirq(&g_plat_crs[0][i++].r, 0, ACPI_ACTIVE_HIGH, 1, 0, gsi,
			1);
	mock_res_end(&g_plat_crs[0][i++].r);
	(void)mock_resources_stride(amd, "_CRS", g_plat_crs[0], i,
				    (int)sizeof(union rslot));
	tp = mock_device(amd, "TPNL", "SYNA2B5D", NULL);
	mock_add_cid(tp, "ACPI0C50");
	i = 0;
	mock_res_i2c(&g_plat_tp[i++].r, 0x15, 400000, 0, "\\_SB.I2CA", 1);
	mock_res_end(&g_plat_tp[i++].r);
	(void)mock_resources_stride(tp, "_CRS", g_plat_tp, i,
				    (int)sizeof(union rslot));
	/* a Bay Trail controller without an interrupt: polled */
	byt = mock_device(NULL, "I2C7", "80860F41", "7");
	i = 0;
	mock_res_addr64(&g_plat_crs[1][i++].r,
			(UINT64)(unsigned long)mb->window,
			(UINT64)(unsigned long)mb->window + 0xfff, 0x1000, 0, 0);
	mock_res_end(&g_plat_crs[1][i++].r);
	(void)mock_resources_stride(byt, "_CRS", g_plat_crs[1], i,
				    (int)sizeof(union rslot));
	mock_gsi_bind(20, m);

	T_EQ(i2c_dw_platform_init(), 2);
	a = i2c_find_adapter_by_acpi_handle(amd);
	ab = i2c_find_adapter_by_acpi_handle(byt);
	T_ASSERT(a && ab);
	dev = i2c_get_adapdata(a);
	T_EQ(dev->clk_khz, 150000);
	T_ASSERT(dev->flags & ACCESS_INTR_MASK);
	T_STR(dev->irq_kind, "GSI");
	T_EQ(m->fs_hcnt, 132); /* 150 MHz fast mode */
	T_EQ(m->fs_lcnt, 239);
	T_EQ(wr(a, 0x50, 0, w, 2, NULL, 0), 1);
	T_EQ(wr(a, 0x50, 0, w, 1, r, 1), 2);
	T_EQ(r[0], 0x3e);
	T_ASSERT(dev->st.irqs > 0);
	c = i2c_find_client(a, 0x15);
	T_ASSERT(c);
	T_STR(c->name, "SYNA2B5D:00");
	i2c_client_put(c);
	/* Bay Trail: 100 MHz (clock parameter register 0), polled */
	dev = i2c_get_adapdata(ab);
	T_EQ(dev->clk_khz, 100000);
	T_ASSERT(dev->flags & ACCESS_POLLING);
	T_ASSERT(dev->flags & ACCESS_NO_IRQ_SUSPEND);
	T_EQ(wr(ab, 0x50, 0, w, 1, r, 1), 2);
	i2c_put_adapter(a);
	i2c_put_adapter(ab);
	i2c_dw_platform_exit();
	T_ASSERT(!i2c_find_adapter_by_acpi_handle(amd));
	mock_gsi_bind(20, NULL);
	dwm_destroy(m);
	dwm_destroy(mb);
	i2c_acpi_invalidate_cache();
}

/* ==========================================================================
 * /dev/i2c-N
 * ========================================================================== */

struct dev_open {
	struct devfs_node *node;
	vfs_file_t *f;
};

static void dev_open(struct i2c_adapter *a, struct dev_open *o)
{
	char path[32];

	ksnprintf(path, sizeof(path), "/dev/i2c-%d", a->nr);
	o->node = mock_devfs_find(path);
	T_ASSERT(o->node);
	T_EQ(o->node->mode, 0600);
	T_EQ(o->node->major, 89);
	T_EQ(o->node->minor, (uint32_t)a->nr);
	o->f = kcalloc(1, sizeof(vfs_file_t));
	T_EQ(o->node->ops->open(o->node, o->f, 0, NULL), 0);
}

static long dev_ioctl(struct dev_open *o, unsigned long req, void *arg)
{
	return o->node->ops->ioctl(o->f, req, arg, NULL);
}

static void dev_close(struct dev_open *o)
{
	o->node->ops->release(o->f);
	kfree(o->f);
}

struct del_ctx {
	struct dw_i2c_dev *dev;
	volatile int done;
};

static void del_fn(void *arg)
{
	struct del_ctx *d = arg;

	dw_del(d->dev);
	__atomic_store_n(&d->done, 1, __ATOMIC_RELEASE);
}

static void t_i2cdev(void)
{
	struct dw_model *m = model_std(16);
	struct dw_i2c_dev *dev = dw_new(m, 100000, 400000);
	struct i2c_adapter *a = &dev->adapter;
	struct dev_open o;
	struct i2c_msg msgs[43];
	struct u_rdwr rd;
	struct u_smbus sm;
	union i2c_smbus_data data;
	unsigned long funcs = 0;
	uint8_t w[3] = { 0x70, 0x0a, 0x0b }, r[40];
	struct i2c_board_info info;
	struct i2c_client *bound;
	struct del_ctx del;

	dev_open(a, &o);
	T_EQ(dev_ioctl(&o, U_I2C_FUNCS, &funcs), 0);
	T_ASSERT(funcs & I2C_FUNC_I2C);
	T_ASSERT(funcs & I2C_FUNC_10BIT_ADDR);
	T_EQ(dev_ioctl(&o, U_I2C_FUNCS, (void *)MOCK_BAD_USER), -EFAULT);
	/* addresses */
	T_EQ(dev_ioctl(&o, U_I2C_SLAVE, (void *)0x80), -EINVAL);
	T_EQ(dev_ioctl(&o, U_I2C_SLAVE, (void *)0x50), 0);
	T_EQ(dev_ioctl(&o, U_I2C_TENBIT, (void *)1), 0);
	T_EQ(dev_ioctl(&o, U_I2C_SLAVE, (void *)0x3ff), 0);
	T_EQ(dev_ioctl(&o, U_I2C_SLAVE, (void *)0x400), -EINVAL);
	T_EQ(dev_ioctl(&o, U_I2C_TENBIT, (void *)0), 0);
	/* an address a driver owns is busy, unless forced */
	kmemset(&info, 0, sizeof(info));
	i2c_strlcpy(info.type, "dummy", sizeof(info.type));
	info.addr = 0x15;
	bound = i2c_new_client_device(a, &info);
	T_ASSERT(!I2C_IS_ERR(bound) && bound->driver);
	T_EQ(dev_ioctl(&o, U_I2C_SLAVE, (void *)0x15), -EBUSY);
	T_EQ(dev_ioctl(&o, U_I2C_SLAVE_FORCE, (void *)0x15), 0);
	i2c_unregister_device(bound);
	T_EQ(dev_ioctl(&o, U_I2C_SLAVE, (void *)0x50), 0);

	/* I2C_RDWR */
	kmemset(msgs, 0, sizeof(msgs));
	rd.msgs = msgs;
	rd.nmsgs = 0;
	T_EQ(dev_ioctl(&o, U_I2C_RDWR, &rd), -EINVAL);
	rd.nmsgs = 43;
	T_EQ(dev_ioctl(&o, U_I2C_RDWR, &rd), -EINVAL);
	rd.msgs = NULL;
	rd.nmsgs = 1;
	T_EQ(dev_ioctl(&o, U_I2C_RDWR, &rd), -EINVAL);
	rd.msgs = (struct i2c_msg *)MOCK_BAD_USER;
	T_EQ(dev_ioctl(&o, U_I2C_RDWR, &rd), -EFAULT);
	T_EQ(dev_ioctl(&o, U_I2C_RDWR, (void *)MOCK_BAD_USER), -EFAULT);
	rd.msgs = msgs;
	msgs[0] = (struct i2c_msg){ 0x50, 0, 3, w };
	msgs[1] = (struct i2c_msg){ 0x50, I2C_M_RD, 2, r };
	rd.nmsgs = 1;
	T_EQ(dev_ioctl(&o, U_I2C_RDWR, &rd), 1);
	msgs[0].len = 1;
	rd.nmsgs = 2;
	kmemset(r, 0, sizeof(r));
	T_EQ(dev_ioctl(&o, U_I2C_RDWR, &rd), 2);
	T_EQ(r[0], 0x0a);
	T_EQ(r[1], 0x0b);
	/* the user's messages are untouched */
	T_ASSERT(msgs[1].buf == r && msgs[1].flags == I2C_M_RD);
	msgs[1].len = 8193;
	T_EQ(dev_ioctl(&o, U_I2C_RDWR, &rd), -EINVAL);
	msgs[1].len = 2;
	msgs[1].buf = (uint8_t *)MOCK_BAD_USER;
	T_EQ(dev_ioctl(&o, U_I2C_RDWR, &rd), -EFAULT);
	msgs[1].buf = r;
	msgs[1].len = 0;
	T_EQ(dev_ioctl(&o, U_I2C_RDWR, &rd), -EOPNOTSUPP);
	/* received length: must be a read with room for a block */
	msgs[1].flags = I2C_M_RECV_LEN;
	msgs[1].len = 40;
	r[0] = 1;
	T_EQ(dev_ioctl(&o, U_I2C_RDWR, &rd), -EINVAL);
	msgs[1].flags = I2C_M_RD | I2C_M_RECV_LEN;
	msgs[1].len = 20;
	T_EQ(dev_ioctl(&o, U_I2C_RDWR, &rd), -EINVAL);
	msgs[1].len = 40;
	r[0] = 0;
	T_EQ(dev_ioctl(&o, U_I2C_RDWR, &rd), -EINVAL);
	/* a real block read through I2C_RDWR from the SMBus device */
	kmemcpy(g_smb->block, "\x01\x02\x03", 3);
	g_smb->block_len = 3;
	w[0] = 0x10;
	msgs[0] = (struct i2c_msg){ 0x60, 0, 1, w };
	msgs[1] = (struct i2c_msg){ 0x60, I2C_M_RD | I2C_M_RECV_LEN, 40, r };
	r[0] = 1;
	T_EQ(dev_ioctl(&o, U_I2C_RDWR, &rd), 2);
	T_EQ(r[0], 3);
	T_EQ(r[3], 3);

	/* I2C_SMBUS */
	T_EQ(dev_ioctl(&o, U_I2C_SLAVE, (void *)0x60), 0);
	sm = (struct u_smbus){ I2C_SMBUS_WRITE, 0x30, I2C_SMBUS_BYTE_DATA,
			       &data };
	data.byte = 0x6b;
	T_EQ(dev_ioctl(&o, U_I2C_SMBUS, &sm), 0);
	sm.read_write = I2C_SMBUS_READ;
	data.byte = 0;
	T_EQ(dev_ioctl(&o, U_I2C_SMBUS, &sm), 0);
	T_EQ(data.byte, 0x6b);
	sm.size = I2C_SMBUS_BLOCK_DATA;
	sm.command = 0x10;
	T_EQ(dev_ioctl(&o, U_I2C_SMBUS, &sm), 0);
	T_EQ(data.block[0], 3);
	sm.size = 42;
	T_EQ(dev_ioctl(&o, U_I2C_SMBUS, &sm), -EINVAL);
	sm.size = I2C_SMBUS_BYTE_DATA;
	sm.read_write = 5;
	T_EQ(dev_ioctl(&o, U_I2C_SMBUS, &sm), -EINVAL);
	sm.read_write = I2C_SMBUS_READ;
	sm.data = NULL;
	T_EQ(dev_ioctl(&o, U_I2C_SMBUS, &sm), -EINVAL);
	sm.data = (union i2c_smbus_data *)MOCK_BAD_USER;
	T_EQ(dev_ioctl(&o, U_I2C_SMBUS, &sm), -EFAULT);
	T_EQ(dev_ioctl(&o, U_I2C_SMBUS, (void *)MOCK_BAD_USER), -EFAULT);
	/* quick with PEC: no data, zero length: the controller refuses */
	T_EQ(dev_ioctl(&o, U_I2C_PEC, (void *)1), 0);
	sm = (struct u_smbus){ I2C_SMBUS_WRITE, 0, I2C_SMBUS_QUICK, NULL };
	T_EQ(dev_ioctl(&o, U_I2C_SMBUS, &sm), -EOPNOTSUPP);
	T_EQ(dev_ioctl(&o, U_I2C_PEC, (void *)0), 0);

	/* read() / write() */
	T_EQ(dev_ioctl(&o, U_I2C_SLAVE, (void *)0x50), 0);
	w[0] = 0x70;
	T_EQ(o.node->ops->write(o.f, w, 1, 0), 1);
	T_EQ(o.node->ops->read(o.f, r, 2, 0), 2);
	T_EQ(r[0], 0x0a);
	T_EQ(o.node->ops->read(o.f, (void *)MOCK_BAD_USER, 2, 0), -EFAULT);
	T_EQ(o.node->ops->write(o.f, (void *)MOCK_BAD_USER, 2, 0), -EFAULT);
	T_EQ(dev_ioctl(&o, U_I2C_SLAVE, (void *)0x33), 0);
	T_EQ(o.node->ops->read(o.f, r, 2, 0), -EREMOTEIO);

	/* adapter parameters */
	T_EQ(dev_ioctl(&o, U_I2C_TIMEOUT, (void *)25), 0);
	T_EQ(a->timeout, 250);
	T_EQ(dev_ioctl(&o, U_I2C_RETRIES, (void *)5), 0);
	T_EQ(a->retries, 5);
	T_EQ(dev_ioctl(&o, U_I2C_TIMEOUT, (void *)0x100000000ul), -EINVAL);
	T_EQ(dev_ioctl(&o, 0x1234, NULL), -ENOTTY);
	a->timeout = 1000;
	a->retries = 3;

	/* deleting the adapter waits for the open file */
	del.dev = dev;
	del.done = 0;
	T_ASSERT(sched_add_task(del_fn, &del, NULL, 0));
	wait_ms(50);
	T_EQ(del.done, 0);
	dev_close(&o);
	while (!del.done)
		wait_ms(1);
	T_ASSERT(!mock_devfs_find("/dev/i2c-0"));
	dwm_destroy(m);
}

/* ==========================================================================
 * GenericSerialBus operation regions
 * ========================================================================== */

static void t_opregion(void)
{
	struct dw_model *m = model_std(16);
	struct mock_node *ctl;
	struct acpi_device *adev;
	struct dw_i2c_dev *dev;
	struct mock_region *reg;
	ACPI_CONNECTION_INFO *ci;
	static uint8_t conn[32];
	uint8_t gsb[40];
	UINT64 *v = (UINT64 *)(void *)gsb;

	mock_ns_reset();
	i2c_acpi_invalidate_cache();
	ctl = mock_device(NULL, "I2CG", "TESTGSB0", NULL);
	adev = acpi_dev_from_handle(ctl);
	dev = dw_new_ex(m, 100000, 400000, 0, adev);
	reg = mock_find_region(ctl, ACPI_ADR_SPACE_GSBUS);
	T_ASSERT(reg && reg->handler);
	ci = reg->context;
	/* I2cSerialBusV2 (0x50, 7-bit, 400 kHz, "\_SB.I2CG") */
	kmemset(conn, 0, sizeof(conn));
	conn[0] = 0x8e;
	conn[1] = 18 + 10;
	conn[3] = 2;
	conn[5] = 1;
	conn[9] = 1;
	conn[10] = 6;
	conn[12] = 0x80;
	conn[13] = 0x1a;
	conn[14] = 0x06;
	conn[16] = 0x50;
	kmemcpy(conn + 18, "\\_SB.I2CG", 10);
	ci->Connection = conn;
	ci->Length = 28;
	g_ee->mem[0x44] = 0x77;
	g_ee->mem[0x45] = 0x88;
	/* byte read */
	kmemset(gsb, 0, sizeof(gsb));
	T_EQ(reg->handler(ACPI_READ | (0x06u << 16), 0x44, 8, v, ci, NULL),
	     AE_OK);
	T_EQ(gsb[0], 0); /* status */
	T_EQ(gsb[2], 0x77);
	/* word write, then read back */
	gsb[2] = 0x34;
	gsb[3] = 0x12;
	T_EQ(reg->handler(ACPI_WRITE | (0x08u << 16), 0x46, 16, v, ci, NULL),
	     AE_OK);
	T_EQ(g_ee->mem[0x46], 0x34);
	T_EQ(g_ee->mem[0x47], 0x12);
	/* multi-byte read of AccessLength bytes */
	ci->AccessLength = 3;
	kmemset(gsb, 0, sizeof(gsb));
	T_EQ(reg->handler(ACPI_READ | (0x0bu << 16), 0x44, 24, v, ci, NULL),
	     AE_OK);
	T_EQ(gsb[2], 0x77);
	T_EQ(gsb[3], 0x88);
	T_EQ(gsb[4], 0x34);
	/* an unsupported protocol */
	T_EQ(reg->handler(ACPI_READ | (0x0cu << 16), 0, 8, v, ci, NULL),
	     AE_BAD_PARAMETER);
	/* a device that is not there: status, not a failure */
	conn[16] = 0x33;
	T_EQ(reg->handler(ACPI_READ | (0x06u << 16), 0, 8, v, ci, NULL),
	     AE_OK);
	T_EQ((int8_t)gsb[0], -EREMOTEIO);
	dw_del(dev);
	T_ASSERT(!mock_find_region(ctl, ACPI_ADR_SPACE_GSBUS));
	acpi_dev_put(adev);
	dwm_destroy(m);
	i2c_acpi_invalidate_cache();
}

/* ==========================================================================
 * The core alone: a scripted adapter
 * ========================================================================== */

static int g_fake_calls, g_fake_eagain;

static int fake_xfer(struct i2c_adapter *a, struct i2c_msg *msgs, int num)
{
	(void)a;
	(void)msgs;
	g_fake_calls++;
	if (g_fake_eagain > 0) {
		g_fake_eagain--;
		return -EAGAIN;
	}
	return num;
}

static uint32_t fake_func(struct i2c_adapter *a)
{
	(void)a;
	return I2C_FUNC_I2C;
}

static const struct i2c_algorithm g_fake_algo = {
	.master_xfer = fake_xfer,
	.functionality = fake_func,
};

static const struct i2c_adapter_quirks g_fake_quirks = {
	.flags = I2C_AQ_COMB_WRITE_THEN_READ,
	.max_comb_1st_msg_len = 2,
	.max_comb_2nd_msg_len = 4,
	.max_read_len = 8,
	.max_write_len = 8,
};

static int g_d1_probes, g_d2_probes, g_d2_removes;

static int d1_probe(struct i2c_client *c)
{
	(void)c;
	g_d1_probes++;
	return -ENODEV; /* not mine after all */
}

static int d2_probe(struct i2c_client *c)
{
	g_d2_probes++;
	i2c_set_clientdata(c, &g_d2_probes);
	return 0;
}

static void d2_remove(struct i2c_client *c)
{
	T_ASSERT(i2c_get_clientdata(c) == &g_d2_probes);
	g_d2_removes++;
}

static const struct i2c_device_id g_sensor_ids[] = {
	{ "sensor", 7 },
	{ "", 0 },
};

/* A driver that defers once, then registers a dummy for its second
 * address from its probe. */
static int g_d3_probes;
static struct i2c_client *g_d3_dummy;

static int d3_probe(struct i2c_client *c)
{
	if (g_d3_probes++ == 0)
		return -EPROBE_DEFER;
	g_d3_dummy = i2c_new_dummy_device(c->adapter, (uint16_t)(c->addr + 1));
	return I2C_IS_ERR(g_d3_dummy) ? (int)I2C_PTR_ERR(g_d3_dummy) : 0;
}

static void d3_remove(struct i2c_client *c)
{
	(void)c;
	i2c_unregister_device(g_d3_dummy);
	g_d3_dummy = NULL;
}

static const struct i2c_device_id g_multi_ids[] = {
	{ "multi", 0 },
	{ "", 0 },
};

static struct i2c_driver g_d3 = { .name = "d3",
				  .probe = d3_probe,
				  .remove = d3_remove,
				  .id_table = g_multi_ids };

static struct i2c_driver g_d1 = { .name = "d1", .probe = d1_probe,
				  .id_table = g_sensor_ids };
static struct i2c_driver g_d2 = { .name = "d2",
				  .probe = d2_probe,
				  .remove = d2_remove,
				  .id_table = g_sensor_ids };

static int g_show_calls;

static long show_reenters(struct i2c_adapter *adap, char *buf, long cap,
			  long pos)
{
	struct i2c_adapter *again = i2c_get_adapter(adap->nr);

	g_show_calls++;
	if (again == adap)
		pos += ksnprintf(buf + pos, (size_t)(cap - pos),
				 "  reentered ok\n");
	if (again)
		i2c_put_adapter(again);
	return pos;
}

static void t_core(void)
{
	struct i2c_adapter a;
	struct i2c_msg msg[3];
	uint8_t b[10];
	struct i2c_board_info info;
	struct i2c_client *c, *c2, *d;
	int w0;

	/* address rules */
	T_EQ(i2c_check_addr_validity(0, 0), -EINVAL);
	T_EQ(i2c_check_addr_validity(0x7f, 0), 0);
	T_EQ(i2c_check_addr_validity(0x80, 0), -EINVAL);
	T_EQ(i2c_check_addr_validity(0, I2C_CLIENT_TEN), 0);
	T_EQ(i2c_check_addr_validity(0x400, I2C_CLIENT_TEN), -EINVAL);
	T_EQ(i2c_check_7bit_addr_validity_strict(0x07), -EINVAL);
	T_EQ(i2c_check_7bit_addr_validity_strict(0x08), 0);
	T_EQ(i2c_check_7bit_addr_validity_strict(0x78), -EINVAL);

	kmemset(&a, 0, sizeof(a));
	i2c_strlcpy(a.name, "scripted", sizeof(a.name));
	a.algo = &g_fake_algo;
	a.quirks = &g_fake_quirks;
	a.retries = 2;
	a.nr = 7;
	T_EQ(i2c_add_numbered_adapter(&a), 0);
	T_EQ(a.nr, 7);
	T_STR(a.dev_name, "i2c-7");
	T_EQ(a.timeout, 1000);
	T_ASSERT(mock_devfs_find("/dev/i2c-7"));
	{
		struct i2c_adapter b2;

		kmemset(&b2, 0, sizeof(b2));
		i2c_strlcpy(b2.name, "x", sizeof(b2.name));
		b2.algo = &g_fake_algo;
		b2.nr = 7;
		T_EQ(i2c_add_numbered_adapter(&b2), -EBUSY);
	}

	/* quirks */
	msg[0] = (struct i2c_msg){ 0x10, 0, 2, b };
	msg[1] = (struct i2c_msg){ 0x10, I2C_M_RD, 4, b };
	msg[2] = (struct i2c_msg){ 0x10, I2C_M_RD, 1, b };
	g_fake_calls = 0;
	T_EQ(i2c_transfer(&a, msg, 2), 2);
	T_EQ(i2c_transfer(&a, msg, 3), -EOPNOTSUPP); /* too many */
	msg[0].len = 3;
	T_EQ(i2c_transfer(&a, msg, 2), -EOPNOTSUPP); /* 1st too long */
	msg[0].len = 2;
	msg[1].addr = 0x11;
	T_EQ(i2c_transfer(&a, msg, 2), -EOPNOTSUPP); /* other address */
	msg[1].addr = 0x10;
	msg[1].flags = 0;
	T_EQ(i2c_transfer(&a, msg, 2), -EOPNOTSUPP); /* 2nd not a read */
	msg[1].flags = I2C_M_RD;
	msg[0].len = 9;
	T_EQ(i2c_transfer(&a, msg, 1), -EOPNOTSUPP); /* single too long */
	msg[0].len = 8;
	T_EQ(i2c_transfer(&a, msg, 1), 1);
	T_EQ(g_fake_calls, 2);
	/* retries on lost arbitration */
	g_fake_eagain = 2;
	T_EQ(i2c_transfer(&a, msg, 1), 1);
	T_EQ(g_fake_calls, 5);
	g_fake_eagain = 5;
	T_EQ(i2c_transfer(&a, msg, 1), -EAGAIN);
	T_EQ(g_fake_calls, 8);
	g_fake_eagain = 0;
	/* bad arguments */
	w0 = g_test_warnings;
	T_EQ(i2c_transfer(&a, msg, 0), -EINVAL);
	T_EQ(i2c_transfer(&a, NULL, 1), -EINVAL);
	msg[0].buf = NULL;
	T_EQ(i2c_transfer(&a, msg, 1), -EINVAL);
	EXPECT_WARNS(w0, 3);
	msg[0].buf = b;
	msg[0].addr = 0x90;
	T_EQ(i2c_transfer(&a, msg, 1), -EINVAL);
	msg[0].addr = 0x10;

	/* clients and binding */
	kmemset(&info, 0, sizeof(info));
	i2c_strlcpy(info.type, "sensor", sizeof(info.type));
	info.addr = 0x48;
	c = i2c_new_client_device(&a, &info);
	T_ASSERT(!I2C_IS_ERR(c));
	T_STR(c->dev_name, "7-0048");
	T_ASSERT(!c->driver);
	T_ASSERT(i2c_verify_client(c) == c);
	T_EQ(I2C_PTR_ERR(i2c_new_client_device(&a, &info)), -EBUSY);
	info.addr = 0x80;
	T_EQ(I2C_PTR_ERR(i2c_new_client_device(&a, &info)), -EINVAL);
	info.addr = 0x48;
	info.flags = I2C_CLIENT_TEN;
	c2 = i2c_new_client_device(&a, &info); /* 10-bit 0x48 is another */
	T_ASSERT(!I2C_IS_ERR(c2));
	T_STR(c2->dev_name, "7-a048");
	i2c_unregister_device(c2);
	/* the first driver declines, the second takes both */
	T_EQ(i2c_register_driver(&g_d1), 0);
	T_EQ(g_d1_probes, 1);
	T_ASSERT(!c->driver);
	T_EQ(i2c_register_driver(&g_d2), 0);
	T_EQ(g_d2_probes, 1);
	T_ASSERT(c->driver == &g_d2);
	T_EQ(i2c_get_match_data(c), 7);
	T_EQ(i2c_register_driver(&g_d2), -EBUSY);
	d = i2c_new_dummy_device(&a, 0x49);
	T_ASSERT(!I2C_IS_ERR(d) && d->driver);
	T_EQ(i2c_check_addr_busy(&a, 0x49, 0, 0), -EBUSY);
	T_EQ(i2c_check_addr_busy(&a, 0x4a, 0, 0), 0);
	/* removing the driver unbinds */
	i2c_del_driver(&g_d2);
	T_EQ(g_d2_removes, 1);
	T_ASSERT(!c->driver);
	i2c_del_driver(&g_d1);
	/* binding again when it comes back */
	T_EQ(i2c_register_driver(&g_d2), 0);
	T_EQ(g_d2_probes, 2);
	/* deferred once; bound later from a driver registration while it
	 * creates a client itself (no recursion into its own bind) */
	T_EQ(i2c_register_driver(&g_d3), 0);
	kmemset(&info, 0, sizeof(info));
	i2c_strlcpy(info.type, "multi", sizeof(info.type));
	info.addr = 0x30;
	c2 = i2c_new_client_device(&a, &info);
	T_ASSERT(!I2C_IS_ERR(c2) && !c2->driver && c2->deferred);
	T_EQ(i2c_register_driver(&g_d1), 0);
	T_EQ(g_d3_probes, 2);
	T_ASSERT(c2->driver == &g_d3 && !c2->deferred);
	T_ASSERT(g_d3_dummy && g_d3_dummy->addr == 0x31);
	i2c_del_driver(&g_d1);
	i2c_del_driver(&g_d3);
	T_ASSERT(!g_d3_dummy);
	i2c_unregister_device(c2);

	/* /proc/i2c: the adapter's show callback may use the registry
	 * itself (no registry lock held around it) */
	{
		static char sbuf[8192];
		long n;

		a.show = show_reenters;
		g_show_calls = 0;
		n = i2c_core_show(sbuf, sizeof(sbuf));
		T_ASSERT(n > 0 && n < (long)sizeof(sbuf));
		T_EQ(g_show_calls, 1);
		T_ASSERT(str_has(sbuf, "reentered ok"));
		a.show = NULL;
	}

	/* the adapter goes: clients unbound and removed */
	i2c_client_get(c);
	i2c_del_adapter(&a);
	T_EQ(g_d2_removes, 2);
	T_ASSERT(!i2c_verify_client(c));
	i2c_client_put(c);
	T_ASSERT(!mock_devfs_find("/dev/i2c-7"));
	T_ASSERT(!i2c_get_adapter(7));
	i2c_del_driver(&g_d2);
	/* deleting it twice is harmless */
	i2c_del_adapter(&a);
}

/* ==========================================================================
 * Randomised stress
 * ========================================================================== */

struct stress_ctx {
	struct i2c_adapter *a;
	struct dw_model *m;
	int id;
	unsigned long ops, bad;
	volatile int done;
	uint64_t until;
};

static void stress_fn(void *arg)
{
	struct stress_ctx *s = arg;
	uint8_t w[64], r[64];

	while (h_now_ns() < s->until) {
		unsigned int op = (unsigned int)(h_rand() % 6);
		int len = 1 + (int)(h_rand() % 40), ret;
		uint8_t base = (uint8_t)(s->id * 64);

		switch (op) {
		case 0: /* write then verify */
			w[0] = (uint8_t)(base + h_rand() % 16);
			for (int i = 1; i <= len && i < 48; i++)
				w[i] = (uint8_t)h_rand();
			if (len > 47)
				len = 47;
			ret = wr(s->a, 0x50, 0, w, len + 1, NULL, 0);
			if (ret == 1) {
				ret = wr(s->a, 0x50, 0, w, 1, r, len);
				if (ret == 2 && kmemcmp(r, w + 1, (size_t)len))
					s->bad++;
			}
			if (ret < 0 && ret != -EAGAIN)
				s->bad++;
			break;
		case 1: /* nobody there */
			ret = wr(s->a, (uint16_t)(0x70 + h_rand() % 8), 0, w, 1,
				 r, 1);
			if (ret != -EREMOTEIO && ret != -EAGAIN)
				s->bad++;
			break;
		case 2: /* data NACK */
			ret = wr(s->a, 0x34, 0, w, 3, NULL, 0);
			if (ret != -EREMOTEIO && ret != -EAGAIN)
				s->bad++;
			break;
		case 3: /* the HID descriptor */
			ret = wr(s->a, 0x15, 0, (const uint8_t *)"\x01\x00", 2,
				 r, 30);
			if (ret == 2 ? kmemcmp(r, g_hid_desc, 30) != 0 :
				       ret != -EAGAIN)
				s->bad++;
			break;
		case 4: /* lost arbitration now and then */
			__atomic_store_n(&s->m->arb_lose, 1, __ATOMIC_RELAXED);
			ret = wr(s->a, 0x50, 0, w, 1, r, 4);
			if (ret != 2 && ret != -EAGAIN)
				s->bad++;
			break;
		case 5: { /* SMBus block */
			struct i2c_client c;

			kmemset(&c, 0, sizeof(c));
			c.adapter = s->a;
			c.addr = 0x60;
			ret = i2c_smbus_read_block_data(&c, 0x10, r);
			if (ret != 6 && ret != -EAGAIN)
				s->bad++;
			break;
		}
		}
		s->ops++;
	}
	__atomic_store_n(&s->done, 1, __ATOMIC_RELEASE);
}

static void t_stress(void)
{
	static const unsigned int depths[] = { 4, 32 };

	for (unsigned int d = 0; d < 2; d++) {
		struct dw_model *m = model_std(depths[d]);
		struct dw_i2c_dev *dev = dw_new(m, 100000, 1000000);
		struct stress_ctx s[3];
		uint64_t until = h_now_ns() + 1500ull * 1000000ull;

		kmemcpy(g_smb->block, "abcdef", 6);
		g_smb->block_len = 6;
		for (int i = 0; i < 3; i++) {
			s[i] = (struct stress_ctx){ &dev->adapter, m, i, 0, 0,
						    0, until };
			T_ASSERT(sched_add_task(stress_fn, &s[i], NULL, 0));
		}
		for (int i = 0; i < 3; i++) {
			while (!__atomic_load_n(&s[i].done, __ATOMIC_ACQUIRE))
				wait_ms(5);
			kprintf("  depth %u thread %d: %lu ops, %lu bad\n",
				depths[d], i, s[i].ops, s[i].bad);
			T_EQ(s[i].bad, 0);
			T_ASSERT(s[i].ops > 10);
		}
		T_EQ(m->rx_over, 0);
		T_EQ(m->tx_over, 0);
		T_EQ(m->rx_under, 0);
		T_EQ(dev->st.timeouts, 0);
		dw_del(dev);
		dwm_destroy(m);
	}
}

#include "host/i2c/test_arb.inc"

/* ==========================================================================
 * Runner
 * ========================================================================== */

static void run(const char *name, void (*fn)(void))
{
	const char *f = TEST_FILTER;

	if (f[0] && !str_has(name, f))
		return;
	test_case(name, fn);
}

int main(void);
int main(void)
{
	kshim_init_main();
	workqueue_init();
	i2c_core_init();
	/* no SMBIOS unless a test loads one */
	dmi_none();
	arb_psp_reset();
	run("formulas", t_formulas);
	run("probe_timings", t_probe_timings);
	run("regaccess", t_regaccess);
	run("basic", t_basic);
	run("hid_and_tenbit", t_hid_and_tenbit);
	run("long", t_long);
	run("smbus", t_smbus);
	run("errors", t_errors);
	run("timeout", t_timeout);
	run("irq_sharing", t_irq_sharing);
	run("concurrent", t_concurrent);
	run("suspend_race", t_suspend_race);
	run("runtime_pm", t_runtime_pm);
	run("atomic", t_atomic);
	run("recovery_busclear", t_recovery_busclear);
	run("recovery_gpio", t_recovery_gpio);
	run("hp_probook", t_hp_probook);
	run("dell_pro", t_dell_pro);
	run("speed", t_speed);
	run("platform", t_platform);
	run("i2cdev", t_i2cdev);
	run("opregion", t_opregion);
	run("core", t_core);
	run("stress", t_stress);
	run("dmi", t_dmi);
	run("iosf", t_iosf);
	run("byt_sem", t_byt_sem);
	run("psp", t_psp);
	run("amd_psp_bus", t_amd_psp_bus);
	run("dmi_quirks", t_dmi_quirks);
	run("lock_ops", t_lock_ops);
	run("suspend_test", t_suspend_test);
	test_summary_and_exit();
}
