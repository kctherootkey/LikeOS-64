// LikeOS I2C - Synopsys DesignWare I2C controller (DW_apb_i2c), master
//
// The controller found as the I2C function of Intel's Low Power
// Subsystem (LPSS: Sunrise Point ... Panther Lake as PCI functions, Bay
// Trail / Cherry Trail / Haswell / Broadwell / Skylake-era parts as ACPI
// devices), of AMD's FCH (AMDI0010...) and of many SoCs.  This header is
// shared by the controller code (common.c: register access, timing,
// enable/disable, power; master.c: the transfer state machine and the
// interrupt handler) and the bus glue (pcidrv.c, platdrv.c) that finds
// controllers, maps them and fills in what the platform knows (input
// clock, timings, quirks).  Nothing here knows what the devices on the
// bus are: clients only ever see the generic I2C core.
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) 2006 Texas Instruments, 2007 MontaVista Software
// Inc., 2009 Provigent Ltd., 2011-2016 Intel Corporation

#ifndef KERNEL_DEV_I2C_BUSSES_I2C_DESIGNWARE_CORE_H
#define KERNEL_DEV_I2C_BUSSES_I2C_DESIGNWARE_CORE_H

#include <kernel/dev/i2c/i2c.h>
#include <kernel/dev/i2c/i2c-core.h>
#include <kernel/ke/completion.h>
#include <kernel/ke/workqueue.h>
#include <kernel/ke/io.h>

struct acpi_device;
struct pci_device;

/* ---- register map ---------------------------------------------------- */

#define DW_IC_CON 0x00
#define DW_IC_TAR 0x04
#define DW_IC_SAR 0x08
#define DW_IC_HS_MADDR 0x0c
#define DW_IC_DATA_CMD 0x10
#define DW_IC_SS_SCL_HCNT 0x14
#define DW_IC_SS_SCL_LCNT 0x18
#define DW_IC_FS_SCL_HCNT 0x1c
#define DW_IC_FS_SCL_LCNT 0x20
#define DW_IC_HS_SCL_HCNT 0x24
#define DW_IC_HS_SCL_LCNT 0x28
#define DW_IC_INTR_STAT 0x2c
#define DW_IC_INTR_MASK 0x30
#define DW_IC_RAW_INTR_STAT 0x34
#define DW_IC_RX_TL 0x38
#define DW_IC_TX_TL 0x3c
#define DW_IC_CLR_INTR 0x40
#define DW_IC_CLR_RX_UNDER 0x44
#define DW_IC_CLR_RX_OVER 0x48
#define DW_IC_CLR_TX_OVER 0x4c
#define DW_IC_CLR_RD_REQ 0x50
#define DW_IC_CLR_TX_ABRT 0x54
#define DW_IC_CLR_RX_DONE 0x58
#define DW_IC_CLR_ACTIVITY 0x5c
#define DW_IC_CLR_STOP_DET 0x60
#define DW_IC_CLR_START_DET 0x64
#define DW_IC_CLR_GEN_CALL 0x68
#define DW_IC_ENABLE 0x6c
#define DW_IC_STATUS 0x70
#define DW_IC_TXFLR 0x74
#define DW_IC_RXFLR 0x78
#define DW_IC_SDA_HOLD 0x7c
#define DW_IC_TX_ABRT_SOURCE 0x80
#define DW_IC_SLV_DATA_NACK_ONLY 0x84
#define DW_IC_DMA_CR 0x88
#define DW_IC_DMA_TDLR 0x8c
#define DW_IC_DMA_RDLR 0x90
#define DW_IC_SDA_SETUP 0x94
#define DW_IC_ACK_GENERAL_CALL 0x98
#define DW_IC_ENABLE_STATUS 0x9c
#define DW_IC_FS_SPKLEN 0xa0
#define DW_IC_HS_SPKLEN 0xa4
#define DW_IC_CLR_RESTART_DET 0xa8
#define DW_IC_SCL_STUCK_AT_LOW_TIMEOUT 0xac
#define DW_IC_SDA_STUCK_AT_LOW_TIMEOUT 0xb0
#define DW_IC_CLR_SCL_STUCK_DET 0xb4
#define DW_IC_DEVICE_ID 0xb8
#define DW_IC_SMBUS_INTR_MASK 0xcc
#define DW_IC_COMP_PARAM_1 0xf4
#define DW_IC_COMP_VERSION 0xf8
#define DW_IC_COMP_TYPE 0xfc
#define DW_IC_REG_SPACE 0x100

/* IC_CON */
#define DW_IC_CON_MASTER (1u << 0)
#define DW_IC_CON_SPEED_STD (1u << 1)
#define DW_IC_CON_SPEED_FAST (2u << 1)
#define DW_IC_CON_SPEED_HIGH (3u << 1)
#define DW_IC_CON_SPEED_MASK (3u << 1)
#define DW_IC_CON_10BITADDR_SLAVE (1u << 3)
#define DW_IC_CON_10BITADDR_MASTER (1u << 4)
#define DW_IC_CON_RESTART_EN (1u << 5)
#define DW_IC_CON_SLAVE_DISABLE (1u << 6)
#define DW_IC_CON_STOP_DET_IFADDRESSED (1u << 7)
#define DW_IC_CON_TX_EMPTY_CTRL (1u << 8)
#define DW_IC_CON_RX_FIFO_FULL_HLD_CTRL (1u << 9)
#define DW_IC_CON_BUS_CLEAR_CTRL (1u << 11)

/* IC_TAR */
#define DW_IC_TAR_10BITADDR_MASTER (1u << 12)

/* IC_DATA_CMD */
#define DW_IC_DATA_CMD_DAT 0xffu
#define DW_IC_DATA_CMD_READ (1u << 8)
#define DW_IC_DATA_CMD_STOP (1u << 9)
#define DW_IC_DATA_CMD_RESTART (1u << 10)

/* interrupt bits (INTR_STAT, INTR_MASK, RAW_INTR_STAT) */
#define DW_IC_INTR_RX_UNDER (1u << 0)
#define DW_IC_INTR_RX_OVER (1u << 1)
#define DW_IC_INTR_RX_FULL (1u << 2)
#define DW_IC_INTR_TX_OVER (1u << 3)
#define DW_IC_INTR_TX_EMPTY (1u << 4)
#define DW_IC_INTR_RD_REQ (1u << 5)
#define DW_IC_INTR_TX_ABRT (1u << 6)
#define DW_IC_INTR_RX_DONE (1u << 7)
#define DW_IC_INTR_ACTIVITY (1u << 8)
#define DW_IC_INTR_STOP_DET (1u << 9)
#define DW_IC_INTR_START_DET (1u << 10)
#define DW_IC_INTR_GEN_CALL (1u << 11)
#define DW_IC_INTR_RESTART_DET (1u << 12)
#define DW_IC_INTR_MST_ON_HOLD (1u << 13)
#define DW_IC_INTR_SCL_STUCK_AT_LOW (1u << 14)

#define DW_IC_INTR_DEFAULT_MASK \
	(DW_IC_INTR_RX_FULL | DW_IC_INTR_TX_ABRT | DW_IC_INTR_STOP_DET)
#define DW_IC_INTR_MASTER_MASK (DW_IC_INTR_DEFAULT_MASK | DW_IC_INTR_TX_EMPTY)

/* IC_ENABLE */
#define DW_IC_ENABLE_ENABLE (1u << 0)
#define DW_IC_ENABLE_ABORT (1u << 1)
#define DW_IC_ENABLE_TX_CMD_BLOCK (1u << 2)
#define DW_IC_ENABLE_SDA_STUCK_RECOVERY (1u << 3)

/* IC_STATUS */
#define DW_IC_STATUS_ACTIVITY (1u << 0)
#define DW_IC_STATUS_TFNF (1u << 1)
#define DW_IC_STATUS_TFE (1u << 2)
#define DW_IC_STATUS_RFNE (1u << 3)
#define DW_IC_STATUS_RFF (1u << 4)
#define DW_IC_STATUS_MASTER_ACTIVITY (1u << 5)
#define DW_IC_STATUS_SLAVE_ACTIVITY (1u << 6)
#define DW_IC_STATUS_MASTER_HOLD_TX_FIFO_EMPTY (1u << 7)
#define DW_IC_STATUS_SDA_STUCK_NOT_RECOVERED (1u << 11)

/* IC_SDA_HOLD */
#define DW_IC_SDA_HOLD_RX_SHIFT 16
#define DW_IC_SDA_HOLD_RX_MASK (0xffu << 16)

/* IC_COMP_PARAM_1 */
#define DW_IC_COMP_PARAM_1_SPEED_MODE_HIGH (3u << 2)
#define DW_IC_COMP_PARAM_1_SPEED_MODE_MASK (3u << 2)
#define DW_IC_FIFO_TX_SHIFT 16 /* bits 23:16: TX depth - 1 */
#define DW_IC_FIFO_RX_SHIFT 8 /* bits 15:8: RX depth - 1 */
#define DW_IC_FIFO_MIN_DEPTH 2

#define DW_IC_COMP_TYPE_VALUE 0x44570140u /* "DW" + 0x0140 */
#define DW_IC_SDA_HOLD_MIN_VERS 0x3131312Au /* "111*": v1.11 */
#define DW_IC_BUS_CLEAR_MIN_VERS 0x3230322Au /* "202*": v2.02 */

/* TX_ABRT_SOURCE bits */
#define ABRT_7B_ADDR_NOACK 0
#define ABRT_10ADDR1_NOACK 1
#define ABRT_10ADDR2_NOACK 2
#define ABRT_TXDATA_NOACK 3
#define ABRT_GCALL_NOACK 4
#define ABRT_GCALL_READ 5
#define ABRT_HS_ACKDET 6
#define ABRT_SBYTE_ACKDET 7
#define ABRT_HS_NORSTRT 8
#define ABRT_SBYTE_NORSTRT 9
#define ABRT_10B_RD_NORSTRT 10
#define ABRT_MASTER_DIS 11
#define ARB_LOST 12
#define ABRT_SLAVE_FLUSH_TXFIFO 13
#define ABRT_SLAVE_ARBLOST 14
#define ABRT_SLAVE_RD_INTX 15
#define ABRT_USER_ABRT 16
#define ABRT_SDA_STUCK_AT_LOW 17
#define DW_IC_ABRT_SOURCES 18

#define DW_IC_TX_ABRT_7B_ADDR_NOACK (1u << ABRT_7B_ADDR_NOACK)
#define DW_IC_TX_ABRT_10ADDR1_NOACK (1u << ABRT_10ADDR1_NOACK)
#define DW_IC_TX_ABRT_10ADDR2_NOACK (1u << ABRT_10ADDR2_NOACK)
#define DW_IC_TX_ABRT_TXDATA_NOACK (1u << ABRT_TXDATA_NOACK)
#define DW_IC_TX_ABRT_GCALL_NOACK (1u << ABRT_GCALL_NOACK)
#define DW_IC_TX_ABRT_GCALL_READ (1u << ABRT_GCALL_READ)
#define DW_IC_TX_ARB_LOST (1u << ARB_LOST)
#define DW_IC_TX_ABRT_ADDR_NOACK                                          \
	(DW_IC_TX_ABRT_7B_ADDR_NOACK | DW_IC_TX_ABRT_10ADDR1_NOACK |      \
	 DW_IC_TX_ABRT_10ADDR2_NOACK)
#define DW_IC_TX_ABRT_NOACK                                               \
	(DW_IC_TX_ABRT_ADDR_NOACK | DW_IC_TX_ABRT_TXDATA_NOACK |          \
	 DW_IC_TX_ABRT_GCALL_NOACK)

/* ---- Intel LPSS private registers ------------------------------------- */

/* Offset of the private block from the controller's registers: PCI
 * functions (Sunrise Point and later) and their ACPI-mode twins put it at
 * 0x200; Bay Trail, Cherry Trail, Lynx Point, Wildcat Point at 0x800. */
#define LPSS_PRIV_OFFSET_PCI 0x200
#define LPSS_PRIV_OFFSET_LEGACY 0x800
#define LPSS_PRIV_SIZE 0x100
#define LPSS_PRIV_REG_COUNT (LPSS_PRIV_SIZE / 4)

#define LPSS_PRIV_CLOCK_PARAMS 0x00 /* legacy: bit 0 clock gate enable */
#define LPSS_PRIV_CLOCK_GATE_EN (1u << 0)
#define LPSS_PRIV_RESETS 0x04
#define LPSS_PRIV_RESETS_FUNC 0x3u /* function and APB out of reset */
#define LPSS_PRIV_RESETS_IDMA (1u << 2)
#define LPSS_PRIV_GENERAL 0x08
#define LPSS_PRIV_ACTIVELTR 0x10
#define LPSS_PRIV_IDLELTR 0x14
#define LPSS_PRIV_REMAP_ADDR 0x40 /* 64-bit, low dword first */
#define LPSS_PRIV_DEVIDLE_CONTROL 0x4c
#define LPSS_PRIV_CAPS 0xfc
#define LPSS_PRIV_CAPS_NO_IDMA (1u << 8)
#define LPSS_PRIV_CAPS_CLK_133 (1u << 9) /* serial clock 133 MHz, else
					  * 120 (datasheet hint) */
#define LPSS_PRIV_CAPS_TYPE_SHIFT 4
#define LPSS_PRIV_CAPS_TYPE_MASK (0xfu << 4)
#define LPSS_DEV_I2C 0

/* ---- flags ------------------------------------------------------------- */

#define ACCESS_INTR_MASK (1u << 0) /* re-arm the mask to re-raise */
#define ACCESS_NO_IRQ_SUSPEND (1u << 1)
#define ARBITRATION_SEMAPHORE (1u << 2) /* bus shared with AMD PSP */
#define ACCESS_POLLING (1u << 3) /* no interrupt: polled transfers */
#define MODEL_AMD_NAVI_GPU (1u << 10)
#define MODEL_WANGXUN_SP (1u << 11)
#define MODEL_MASK (0xfu << 8)

/* Software status (dev->status) */
#define DW_STATUS_ACTIVE (1u << 0)
#define DW_STATUS_WRITE_IN_PROGRESS (1u << 1)
#define DW_STATUS_READ_IN_PROGRESS (1u << 2)
/* A message after the first has started and its first command (which
 * carries the RESTART) is not written yet. */
#define DW_STATUS_NEED_RESTART (1u << 3)
#define DW_STATUS_MASK 0xfu

#define DW_IC_ERR_TX_ABRT 0x1

#define DW_IC_DEFAULT_FUNCTIONALITY                                       \
	(I2C_FUNC_I2C | I2C_FUNC_SMBUS_BYTE | I2C_FUNC_SMBUS_BYTE_DATA |  \
	 I2C_FUNC_SMBUS_WORD_DATA | I2C_FUNC_SMBUS_BLOCK_DATA |           \
	 I2C_FUNC_SMBUS_I2C_BLOCK)

#define DW_IC_DEFAULT_BUS_CAPACITANCE_PF 100
#define DW_IC_DEFAULT_AUTOSUSPEND_MS 1000

/* Bus timings from firmware (nanoseconds; 0: not given). */
struct i2c_timings {
	uint32_t bus_freq_hz;
	uint32_t scl_rise_ns;
	uint32_t scl_fall_ns;
	uint32_t scl_int_delay_ns;
	uint32_t sda_fall_ns;
	uint32_t sda_hold_ns;
};

/* Register access modes (detected from IC_COMP_TYPE). */
enum dw_access {
	DW_ACCESS_32 = 0, /* normal 32-bit */
	DW_ACCESS_SWAP, /* 32-bit, byte swapped */
	DW_ACCESS_16, /* two 16-bit accesses per register */
};

struct dw_i2c_stats {
	uint64_t irqs; /* handler entered */
	uint64_t irq_none; /* not ours (shared line, disabled, gone) */
	uint64_t irq_unexpected; /* ours with no transfer active */
	uint64_t xfers; /* transaction parts started */
	uint64_t timeouts;
	uint64_t aborts;
	uint64_t abort_src[DW_IC_ABRT_SOURCES];
	uint64_t spurious_stop;
	uint64_t early_term;
	uint64_t active_after; /* controller active after the transfer */
	uint64_t bus_busy; /* bus busy before a transfer */
	uint64_t disable_timeouts;
	uint64_t hw_bus_clear;
	uint64_t polled; /* parts run polled (atomic or no irq) */
	uint64_t rpm_suspends, rpm_resumes;
	uint64_t reinits; /* controller re-initialised after a failure */
};

struct dw_i2c_dev {
	struct i2c_adapter adapter;
	char name[48]; /* "i2c_designware 0000:00:15.0" */

	/* ---- the glue fills these before i2c_dw_probe() ---- */
	volatile uint8_t *base; /* controller registers */
	volatile uint8_t *lpss_priv; /* Intel LPSS private block, or NULL */
	struct acpi_device *acpi_dev; /* firmware node (reference owned by
				       * the glue), or NULL */
	uint32_t flags; /* ACCESS_*, MODEL_*, ARBITRATION_SEMAPHORE */
	int irq; /* interrupt number, <= 0: none (polled) */
	const char *irq_kind; /* "MSI", "INTx", "GSI" for logs */
	/* The input clock in kHz, 0 when unknown (then timing registers are
	 * kept as firmware left them unless ACPI provides counts). */
	uint32_t (*get_clk_rate_khz)(struct dw_i2c_dev *dev);
	uint32_t clk_khz; /* what get_clk_rate_khz returns for fixed ones */
	struct i2c_timings timings;
	uint32_t bus_capacitance_pF;
	int clk_freq_optimized;
	int emptyfifo_hold_master; /* holds SCL on an empty TX FIFO */
	/* Count values; non-zero ones are used instead of computed ones. */
	uint16_t ss_hcnt, ss_lcnt;
	uint16_t fs_hcnt, fs_lcnt;
	uint16_t fp_hcnt, fp_lcnt;
	uint16_t hs_hcnt, hs_lcnt;
	uint32_t sda_hold_time; /* IC_SDA_HOLD value, 0: keep */
	unsigned int tx_fifo_depth, rx_fifo_depth; /* 0: from COMP_PARAM_1 */
	/* Platform power hooks around runtime/system suspend (LPSS private
	 * registers), optional. */
	void (*glue_suspend)(struct dw_i2c_dev *dev);
	void (*glue_resume)(struct dw_i2c_dev *dev);
	void *glue;
	unsigned int autosuspend_ms; /* 0: no runtime suspend */

	/* ---- the driver's ---- */
	enum dw_access access;
	uint32_t comp_version;
	uint32_t comp_param1;
	uint32_t master_cfg;
	uint32_t functionality;
	uint32_t fs_spklen, hs_spklen; /* as programmed (read back) */
	int irq_requested;

	/* Transfer state: written by the transfer thread while the
	 * interrupt handler cannot run on it (masked) or under `lock';
	 * the handler works on it under `lock'. */
	spinlock_t lock;
	struct completion cmd_complete;
	int cmd_err;
	struct i2c_msg *msgs;
	int msgs_num;
	int msg_write_idx;
	uint32_t tx_buf_len;
	uint8_t *tx_buf;
	int msg_read_idx;
	uint32_t rx_buf_len;
	uint8_t *rx_buf;
	int msg_err;
	unsigned int status;
	unsigned int abort_source;
	unsigned int sw_mask; /* the interrupt mask in software */
	int rx_outstanding;
	int polling_now; /* this part is polled: the handler keeps out */
	int atomic_now; /* no sleeping in this transfer */

	/* Hardware semaphore (bus shared with firmware/PUNIT/PSP). */
	int (*acquire_lock)(struct dw_i2c_dev *dev);
	void (*release_lock)(struct dw_i2c_dev *dev);
	int shared_with_punit;
	int semaphore_idx;
	/* The bus is shared with firmware (ACPI _SEM, PSP-arbitrated id):
	 * set by the semaphore probes as soon as they know, so the glue
	 * can refuse the adapter when the arbitration is not available. */
	int needs_arbitration;

	/* Bus recovery. */
	struct i2c_bus_recovery_info rinfo;
	int hw_bus_clear; /* IC_ENABLE.SDA_STUCK_RECOVERY usable */

	/* Runtime power: the controller is disabled (and LPSS put in
	 * reset) after autosuspend_ms of idleness; the next transfer
	 * re-initialises it.  rpm_* change under the bus lock. */
	struct delayed_work rpm_work;
	int rpm_enabled;
	volatile int rpm_suspended;
	int rpm_usage;
	uint64_t last_busy_ns;
	int sys_suspended;

	struct dw_i2c_stats st;
	struct i2c_ratelimit rl; /* failure messages (bus lock) */
};

/* ---- register access (common.c) ------------------------------------------ */

/* MMIO accessors; the host test harness substitutes its register model. */
#ifndef dw_mmio_readl
#define dw_mmio_readl(a) readl(a)
#define dw_mmio_writel(v, a) writel((v), (a))
#define dw_mmio_readw(a) readw(a)
#define dw_mmio_writew(v, a) writew((v), (a))
#endif

uint32_t dw_readl(struct dw_i2c_dev *dev, unsigned int reg);
void dw_writel(struct dw_i2c_dev *dev, uint32_t val, unsigned int reg);
static inline void dw_update_bits(struct dw_i2c_dev *dev, unsigned int reg,
				  uint32_t mask, uint32_t val)
{
	uint32_t v = dw_readl(dev, reg);

	dw_writel(dev, (v & ~mask) | (val & mask), reg);
}

static inline uint32_t lpss_priv_readl(struct dw_i2c_dev *dev,
				       unsigned int off)
{
	return dw_mmio_readl(dev->lpss_priv + off);
}
static inline void lpss_priv_writel(struct dw_i2c_dev *dev, uint32_t v,
				    unsigned int off)
{
	dw_mmio_writel(v, dev->lpss_priv + off);
}

/* ---- common.c -------------------------------------------------------------- */

/* hcnt = round(ic_clk * (tSYMBOL + tf) / 10^6) - 3 + offset
 * lcnt = round(ic_clk * (tLOW + tf) / 10^6) - 1 + offset
 * (ic_clk in kHz, times in ns).  With ic_clk 0 the register's current
 * value is returned. */
uint32_t i2c_dw_scl_hcnt(struct dw_i2c_dev *dev, unsigned int reg,
			 uint32_t ic_clk, uint32_t tSYMBOL, uint32_t tf,
			 int offset);
uint32_t i2c_dw_scl_lcnt(struct dw_i2c_dev *dev, unsigned int reg,
			 uint32_t ic_clk, uint32_t tLOW, uint32_t tf,
			 int offset);
uint32_t i2c_dw_clk_rate(struct dw_i2c_dev *dev);
/* Detect the register access mode from IC_COMP_TYPE: 0 or -ENODEV. */
int i2c_dw_init_regmap(struct dw_i2c_dev *dev);
/* Timings and bus speed from firmware: _DSD properties over the glue's
 * defaults, the ACPI bus speed of the clients, SSCN/FMCN/FPCN/HSCN
 * counts.  0 or -EINVAL (unsupported speed). */
int i2c_dw_fw_parse_and_configure(struct dw_i2c_dev *dev);
void i2c_dw_configure_master(struct dw_i2c_dev *dev);
int i2c_dw_acquire_lock(struct dw_i2c_dev *dev);
void i2c_dw_release_lock(struct dw_i2c_dev *dev);
/* Sleep (or spin in atomic transfers) between `min_us' and `max_us'. */
void i2c_dw_delay(struct dw_i2c_dev *dev, unsigned long min_us,
		  unsigned long max_us);
int i2c_dw_wait_bus_not_busy(struct dw_i2c_dev *dev);
int i2c_dw_handle_tx_abort(struct dw_i2c_dev *dev);
uint32_t i2c_dw_func(struct i2c_adapter *adap);
void __i2c_dw_disable(struct dw_i2c_dev *dev);
void i2c_dw_disable(struct dw_i2c_dev *dev);
/* Disable, program timings, SDA hold, FIFO thresholds and IC_CON. */
int i2c_dw_init(struct dw_i2c_dev *dev);
/* Register the adapter: access mode, SDA hold, FIFO depths, master
 * timings, controller init, interrupt, i2c_add_numbered_adapter.
 * 0 or -errno (everything undone). */
int i2c_dw_probe(struct dw_i2c_dev *dev);
/* Unregister and quiesce (the glue frees afterwards). */
void i2c_dw_remove(struct dw_i2c_dev *dev);
/* Interrupt handler (IRQ_NONE / IRQ_HANDLED). */
int i2c_dw_isr(void *arg);
/* /proc/i2c details. */
long i2c_dw_show(struct i2c_adapter *adap, char *buf, long cap, long pos);
/* Human-readable abort cause. */
const char *i2c_dw_abort_name(int bit);

/* Runtime power: get before touching the hardware in a transfer, put
 * after (bus lock held). */
void i2c_dw_rpm_get(struct dw_i2c_dev *dev);
void i2c_dw_rpm_put(struct dw_i2c_dev *dev);
void i2c_dw_rpm_enable(struct dw_i2c_dev *dev, unsigned int delay_ms);
/* Run the idle check now (tests; the work item does it after the
 * delay). */
void i2c_dw_rpm_idle_check(struct dw_i2c_dev *dev);
/* System suspend/resume of the controller (adapter pm ops). */
int i2c_dw_suspend(struct dw_i2c_dev *dev);
int i2c_dw_resume(struct dw_i2c_dev *dev);
void i2c_dw_shutdown(struct dw_i2c_dev *dev);

/* ---- master.c -------------------------------------------------------------- */

int i2c_dw_set_timings_master(struct dw_i2c_dev *dev);
int i2c_dw_probe_master(struct dw_i2c_dev *dev);
int i2c_dw_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
int i2c_dw_xfer_atomic(struct i2c_adapter *adap, struct i2c_msg *msgs,
		       int num);
int i2c_dw_isr_master(struct dw_i2c_dev *dev);
void i2c_dw_release_recovery(struct dw_i2c_dev *dev);

/* ---- semaphores (baytrail.c, amdpsp.c) -------------------------------------- */

/* 0 attached, -ENODEV no semaphore for this bus, -errno (the bus needs
 * one and it is not available: dev->needs_arbitration is set). */
int i2c_dw_baytrail_probe_lock_support(struct dw_i2c_dev *dev);
int i2c_dw_amdpsp_probe_lock_support(struct dw_i2c_dev *dev);
/* All of them in turn: 0 (none needed, or one attached) or the error of
 * the one the bus needs. */
int i2c_dw_probe_lock_support(struct dw_i2c_dev *dev);
/* Undo an attached semaphore (adapter gone). */
void i2c_dw_remove_lock_support(struct dw_i2c_dev *dev);
void i2c_dw_amdpsp_remove_lock_support(struct dw_i2c_dev *dev);
/* /proc/i2c lines of the semaphores. */
long i2c_dw_baytrail_show(struct dw_i2c_dev *dev, char *buf, long cap,
			  long pos);
long i2c_dw_amdpsp_show(struct dw_i2c_dev *dev, char *buf, long cap, long pos);

/* ---- glue entry points ------------------------------------------------------- */

/* Register the PCI driver (LPSS and older PCI controllers). */
int i2c_dw_pci_init(void);
/* Probe the ACPI-enumerated controllers. */
int i2c_dw_platform_init(void);
/* Unregister them all / the PCI driver (tests, teardown). */
void i2c_dw_platform_exit(void);
void i2c_dw_pci_exit(void);
/* Both, in the order the boot wants them. */
void i2c_designware_init(void);

/* Intel LPSS private block: bring the function out of reset, set the
 * remap address (the BAR) -- the sequence for the PCI-style block at
 * 0x200; save/restore around power-down. */
void i2c_dw_lpss_init_priv(struct dw_i2c_dev *dev, uint64_t bar_phys);
void i2c_dw_lpss_suspend(struct dw_i2c_dev *dev);
void i2c_dw_lpss_resume(struct dw_i2c_dev *dev);

#endif /* KERNEL_DEV_I2C_BUSSES_I2C_DESIGNWARE_CORE_H */
