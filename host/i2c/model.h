/* A register-level model of a Synopsys DesignWare I2C master and of the
 * bus behind it, for the I2C harness (host/test-i2c.sh).
 *
 * The model has the controller's register file (component parameters,
 * version and type; the 32-bit, byte-swapped or 16-bit bus as configured),
 * its TX FIFO of data/command entries and RX FIFO, IC_ENABLE with a
 * disable latency in IC_ENABLE_STATUS, the latched and level interrupt
 * causes with their mask and read-to-clear registers, aborts with their
 * source bits (the TX FIFO held flushed until IC_CLR_TX_ABRT), an Intel
 * LPSS private block (the controller only works out of reset), and the
 * bus-clear feature.  A bus thread per controller executes one FIFO entry
 * at a time against the target models on the bus -- START, address,
 * RESTART on the entry's bit or a direction change, data, STOP on the
 * entry's bit (or, without "hold on empty FIFO", when the FIFO runs dry)
 * -- logging every bus event, so tests can check what happened on the
 * wire.  Faults: address/data NACK, clock stretching, lost arbitration, a
 * target holding SDA low (recoverable by the bus clear or by clocking SCL
 * through GPIOs), a controller that disappears (all-ones).
 *
 * Copyright (C) 2026 The LikeOS Project */
#ifndef HOST_I2C_MODEL_H
#define HOST_I2C_MODEL_H

/* The console's va_list printf (the prelude lacks it). */
int kvsnprintf(char *buf, size_t size, const char *fmt, __builtin_va_list ap);

#define DWM_MAX_SLAVES 16
#define DWM_LOG 8192

enum { DWM_ACC_32 = 0, DWM_ACC_SWAP, DWM_ACC_16 };

/* Bus events. */
enum {
	EV_START = 1,
	EV_RESTART,
	EV_ADDR, /* val: addr | read << 15 | ten << 14 */
	EV_NACK_ADDR,
	EV_WRITE, /* val: byte */
	EV_READ, /* val: byte */
	EV_NACK_DATA,
	EV_STOP,
	EV_ABORT, /* val: abort source bit */
	EV_ARB_LOST,
	EV_RECOVER,
};

struct dwm_event {
	uint8_t type;
	uint16_t val;
};

enum {
	SLV_REGMEM = 1, /* 8-bit register pointer, 256 bytes */
	SLV_HID, /* 16-bit registers: write reg, then read from it */
	SLV_SMBUS, /* SMBus commands with optional PEC */
	SLV_CB, /* a device model of a test: the cb_* callbacks */
};

struct dwm_slave {
	int kind;
	uint16_t addr;
	int ten;
	int present;
	unsigned int stretch_us; /* per byte */
	int nack_after; /* NACK the Nth written data byte (0: never) */
	int pec; /* SMBUS: append/check PEC */
	/* state of the transaction */
	int wcount, rcount, read_dir;
	uint8_t ptr;
	uint16_t reg;
	uint8_t cmd;
	uint8_t crc;
	uint8_t crc_at_stop; /* the running PEC when the STOP came */
	uint8_t mem[512];
	uint8_t block[34]; /* SMBUS block data per command */
	uint8_t block_len;
	int bad_count; /* SMBUS: return this as block count instead */
	int corrupt_pec; /* SMBUS: send a wrong PEC */
	unsigned long txns, bytes_in, bytes_out;
	/* SLV_CB: called on the bus thread with the model's lock held.
	 * cb_start: address phase (restart: a repeated START), 1 ACK / 0
	 * NACK; cb_write: a data byte, 1 ACK / 0 NACK; cb_read: the next
	 * byte; cb_stop: the STOP (also after an abort that ends the
	 * transaction). */
	int (*cb_start)(struct dwm_slave *s, int read, int restart);
	int (*cb_write)(struct dwm_slave *s, uint8_t b);
	uint8_t (*cb_read)(struct dwm_slave *s);
	void (*cb_stop)(struct dwm_slave *s);
	void *cb_priv;
};

struct dw_model {
	uint8_t window[4096]; /* the driver's register addresses */
	spinlock_t lock;
	/* configuration */
	int access;
	uint32_t comp_type, version, param1;
	unsigned int tx_depth, rx_depth;
	int hold_on_empty;
	int lpss; /* private block at 0x200, reset must be released */
	uint32_t lpss_caps;
	int bus_clear_hw; /* IC_ENABLE bit 3 works */
	int disable_latency; /* ENABLE_STATUS reads that still say 1 */
	unsigned int tick_us;
	/* registers */
	uint32_t con, tar, intr_mask, rx_tl, tx_tl, enable, sda_hold;
	uint32_t ss_hcnt, ss_lcnt, fs_hcnt, fs_lcnt, hs_hcnt, hs_lcnt;
	uint32_t fs_spklen, hs_spklen, smbus_intr_mask;
	uint32_t raw_latched, abrt_source;
	int abrt_hold, enable_status_count, recovery_pending, sda_not_recovered;
	uint32_t priv[64];
	uint32_t r16_cache[64];
	uint16_t w16_stash[64];
	/* FIFOs */
	uint32_t tx[256];
	unsigned int tx_head, tx_count;
	uint8_t rx[256];
	unsigned int rx_head, rx_count;
	/* bus */
	int in_txn, cur_read, hold, stretched;
	struct dwm_slave *cur;
	int gone;
	int stuck_sda; /* a target holds SDA low */
	int stuck_clocks; /* SCL pulses that free it (GPIO recovery) */
	int arb_lose; /* lose arbitration on the next N STARTs */
	int scl_gpio, sda_gpio_drive; /* recovery GPIO levels */
	struct dwm_slave *slaves[DWM_MAX_SLAVES];
	int nslaves;
	struct dwm_event log[DWM_LOG];
	int nlog;
	/* statistics */
	unsigned long rx_over, tx_over, rx_under, starts, stops, aborts,
		max_txflr, cmds, disabled_while_active;
	/* the bus thread */
	volatile int running, exited;
	int irq; /* interrupt number its line raises */
	/* Called (model lock held) at every START and repeated START: the
	 * arbitration models check the bus is the OS's then. */
	void (*on_start)(struct dw_model *m);
	int on_start_which;
};

struct dw_model *dwm_create(unsigned int tx_depth, unsigned int rx_depth);
void dwm_destroy(struct dw_model *m);
void dwm_add_slave(struct dw_model *m, struct dwm_slave *s);
struct dwm_slave *dwm_slave_new(int kind, uint16_t addr, int ten);
int dwm_irq_line(struct dw_model *m);
void dwm_log_clear(struct dw_model *m);
/* The events as text ("S 15W w01 w00 Sr 15R r1e ... P"), into buf. */
void dwm_log_text(struct dw_model *m, char *buf, int cap);
uint8_t dwm_crc8(uint8_t crc, uint8_t b);

/* MMIO accessors the driver is compiled against. */
uint32_t model_readl(const volatile void *a);
void model_writel(uint32_t v, volatile void *a);
uint16_t model_readw(const volatile void *a);
void model_writew(uint16_t v, volatile void *a);

/* GPIO recovery lines (host/i2c/mocks.c). */
int dwm_get_scl(struct dw_model *m);
int dwm_get_sda(struct dw_model *m);
void dwm_set_scl(struct dw_model *m, int v);
void dwm_set_sda(struct dw_model *m, int v);

#endif
