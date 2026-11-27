// LikeOS I2C - DesignWare controller: master transfers
//
// A transfer is split at messages carrying I2C_M_STOP into parts; each
// part is one bus transaction to one target address.  For a part:
//
//   1. wait until the bus is idle (IC_STATUS.ACTIVITY; recovery if it
//      stays busy for 20 ms);
//   2. disable the controller, program the target address (10-bit when
//      the message says so), enable it, clear and unmask TX_EMPTY,
//      RX_FULL, TX_ABRT, STOP_DET;
//   3. the interrupt handler (or, polled, the transfer thread itself)
//      keeps the TX FIFO filled: one IC_DATA_CMD entry per byte written
//      and one read command per byte to read, never more than the FIFO
//      has room for and never more reads queued than the RX FIFO can
//      take; a RESTART on the first byte of every message after the
//      first, a STOP on the last byte of the last message; received
//      bytes are drained on RX_FULL.  A message with I2C_M_RECV_LEN
//      queues only its length byte until that byte arrives, then the rest;
//   4. the part ends with STOP_DET (or TX_ABRT) once nothing is left to
//      receive; the thread then disables the controller (after waiting
//      for it to go idle; recovering if it does not).
//
// A part that does not end within adapter->timeout resets the
// controller and runs bus recovery: -ETIMEDOUT.  An abort is decoded
// into an errno (address or data NACK -EREMOTEIO, lost arbitration
// -EAGAIN which the core retries, otherwise -EIO).
//
// Concurrency: the transfer thread holds the adapter's bus lock, so one
// transfer at a time.  The interrupt handler works on the transfer state
// under dev->lock; the thread writes that state before it unmasks the
// interrupts and takes dev->lock to end a part (mask, clear ACTIVE) so
// the handler never touches messages that the caller is about to reuse.
// A polled part (no interrupt line, or an atomic transfer with interrupts
// off) masks the interrupts in hardware and processes the status bits in
// the thread; the handler stays out (dev->polling_now) should a shared
// line call it meanwhile.
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) 2006 Texas Instruments, 2007 MontaVista Software
// Inc., 2009 Provigent Ltd., 2011-2016 Intel Corporation

#include <kernel/dev/i2c/busses/i2c-designware-core.h>
#include <kernel/dev/i2c/i2c-core.h>
#include <kernel/dev/gpio/gpiolib.h>
#include <kernel/hal/acpi_dev.h>
#include <kernel/ke/irq.h>
#include <kernel/ke/delay.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/ke/syscall.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

/* ==========================================================================
 * Timings
 * ========================================================================== */

int i2c_dw_set_timings_master(struct dw_i2c_dev *dev)
{
	struct i2c_timings *t = &dev->timings;
	uint32_t sda_falling_time, scl_falling_time, comp_param1, ic_clk;
	int ret;

	ret = i2c_dw_acquire_lock(dev);
	if (ret)
		return ret;
	comp_param1 = dw_readl(dev, DW_IC_COMP_PARAM_1);
	i2c_dw_release_lock(dev);

	/* Fall times: 300 ns unless firmware says otherwise. */
	sda_falling_time = t->sda_fall_ns ? t->sda_fall_ns : 300;
	scl_falling_time = t->scl_fall_ns ? t->scl_fall_ns : 300;

	/* Standard mode, unless counts are given. */
	if (!dev->ss_hcnt || !dev->ss_lcnt) {
		ic_clk = i2c_dw_clk_rate(dev);
		dev->ss_hcnt = (uint16_t)i2c_dw_scl_hcnt(
			dev, DW_IC_SS_SCL_HCNT, ic_clk,
			4000, /* tHD;STA = tHIGH = 4.0 us */
			sda_falling_time, 0);
		dev->ss_lcnt = (uint16_t)i2c_dw_scl_lcnt(
			dev, DW_IC_SS_SCL_LCNT, ic_clk, 4700, /* tLOW = 4.7 us */
			scl_falling_time, 0);
	}

	/* Fast mode plus uses the fast mode registers with its own
	 * values. */
	if (t->bus_freq_hz == I2C_MAX_FAST_MODE_PLUS_FREQ) {
		if (dev->fp_hcnt && dev->fp_lcnt) {
			dev->fs_hcnt = dev->fp_hcnt;
			dev->fs_lcnt = dev->fp_lcnt;
		} else {
			ic_clk = i2c_dw_clk_rate(dev);
			dev->fs_hcnt = (uint16_t)i2c_dw_scl_hcnt(
				dev, DW_IC_FS_SCL_HCNT, ic_clk,
				260, /* tHIGH = 260 ns */
				sda_falling_time, 0);
			dev->fs_lcnt = (uint16_t)i2c_dw_scl_lcnt(
				dev, DW_IC_FS_SCL_LCNT, ic_clk,
				500, /* tLOW = 500 ns */
				scl_falling_time, 0);
		}
	}

	/* Fast mode; needed in high speed mode as well. */
	if (!dev->fs_hcnt || !dev->fs_lcnt) {
		ic_clk = i2c_dw_clk_rate(dev);
		dev->fs_hcnt = (uint16_t)i2c_dw_scl_hcnt(
			dev, DW_IC_FS_SCL_HCNT, ic_clk,
			600, /* tHD;STA = tHIGH = 0.6 us */
			sda_falling_time, 0);
		dev->fs_lcnt = (uint16_t)i2c_dw_scl_lcnt(
			dev, DW_IC_FS_SCL_LCNT, ic_clk, 1300, /* tLOW = 1.3 us */
			scl_falling_time, 0);
	}

	/* High speed when the controller has it, else fast mode. */
	if ((dev->master_cfg & DW_IC_CON_SPEED_MASK) == DW_IC_CON_SPEED_HIGH) {
		if ((comp_param1 & DW_IC_COMP_PARAM_1_SPEED_MODE_MASK) !=
		    DW_IC_COMP_PARAM_1_SPEED_MODE_HIGH) {
			kprintf("%s: High Speed not supported!\n", dev->name);
			t->bus_freq_hz = I2C_MAX_FAST_MODE_FREQ;
			dev->master_cfg &= ~DW_IC_CON_SPEED_MASK;
			dev->master_cfg |= DW_IC_CON_SPEED_FAST;
			dev->hs_hcnt = 0;
			dev->hs_lcnt = 0;
		} else if (!dev->hs_hcnt || !dev->hs_lcnt) {
			uint32_t t_high, t_low;

			/* The databook's values for 100 pF and 400 pF bus
			 * capacitance (the only legal ones). */
			if (dev->bus_capacitance_pF >= 400) {
				t_high = dev->clk_freq_optimized ? 160 : 120;
				t_low = 320;
			} else {
				t_high = 60;
				t_low = dev->clk_freq_optimized ? 120 : 160;
			}
			ic_clk = i2c_dw_clk_rate(dev);
			dev->hs_hcnt = (uint16_t)i2c_dw_scl_hcnt(
				dev, DW_IC_HS_SCL_HCNT, ic_clk, t_high,
				sda_falling_time, 0);
			dev->hs_lcnt = (uint16_t)i2c_dw_scl_lcnt(
				dev, DW_IC_HS_SCL_LCNT, ic_clk, t_low,
				scl_falling_time, 0);
		}
	}
	return 0;
}

void i2c_dw_configure_master(struct dw_i2c_dev *dev)
{
	struct i2c_timings *t = &dev->timings;

	dev->functionality |= I2C_FUNC_10BIT_ADDR | DW_IC_DEFAULT_FUNCTIONALITY;
	if ((dev->flags & MODEL_MASK) != MODEL_AMD_NAVI_GPU)
		dev->functionality |= I2C_FUNC_PROTOCOL_MANGLING;

	dev->master_cfg = DW_IC_CON_MASTER | DW_IC_CON_SLAVE_DISABLE |
			  DW_IC_CON_RESTART_EN;

	switch (t->bus_freq_hz) {
	case I2C_MAX_STANDARD_MODE_FREQ:
		dev->master_cfg |= DW_IC_CON_SPEED_STD;
		break;
	case I2C_MAX_HIGH_SPEED_MODE_FREQ:
		dev->master_cfg |= DW_IC_CON_SPEED_HIGH;
		break;
	default:
		dev->master_cfg |= DW_IC_CON_SPEED_FAST;
	}
}

/* ==========================================================================
 * Interrupt mask and status (dev->lock held, or the handler kept out)
 * ========================================================================== */

static int dw_polled(const struct dw_i2c_dev *dev)
{
	return (dev->flags & ACCESS_POLLING) || dev->polling_now;
}

static void dw_write_intr_mask(struct dw_i2c_dev *dev, uint32_t intr_mask)
{
	/* Polled: the hardware mask stays 0, the software one tracks. */
	dw_writel(dev, dw_polled(dev) ? 0 : intr_mask, DW_IC_INTR_MASK);
	dev->sw_mask = intr_mask;
}

static uint32_t dw_read_intr_mask(struct dw_i2c_dev *dev)
{
	if (!dw_polled(dev))
		return dw_readl(dev, DW_IC_INTR_MASK);
	return dev->sw_mask;
}

static void dw_enable(struct dw_i2c_dev *dev)
{
	dev->status |= DW_STATUS_ACTIVE;
	dw_writel(dev, 1, DW_IC_ENABLE);
}

static void dw_disable_nowait(struct dw_i2c_dev *dev)
{
	dw_writel(dev, 0, DW_IC_ENABLE);
	dev->status &= ~DW_STATUS_ACTIVE;
}

static uint32_t i2c_dw_read_clear_intrbits(struct dw_i2c_dev *dev)
{
	uint32_t stat;

	/* IC_INTR_STAT is IC_RAW_INTR_STAT & IC_INTR_MASK; polled, the
	 * software mask stands in for the hardware one. */
	if (!dw_polled(dev)) {
		stat = dw_readl(dev, DW_IC_INTR_STAT);
	} else {
		stat = dw_readl(dev, DW_IC_RAW_INTR_STAT);
		stat &= dev->sw_mask;
	}

	/* Each cause through its own clear register: IC_CLR_INTR would
	 * also clear causes raised after the status was read. */
	if (stat & DW_IC_INTR_RX_UNDER)
		(void)dw_readl(dev, DW_IC_CLR_RX_UNDER);
	if (stat & DW_IC_INTR_RX_OVER)
		(void)dw_readl(dev, DW_IC_CLR_RX_OVER);
	if (stat & DW_IC_INTR_TX_OVER)
		(void)dw_readl(dev, DW_IC_CLR_TX_OVER);
	if (stat & DW_IC_INTR_RD_REQ)
		(void)dw_readl(dev, DW_IC_CLR_RD_REQ);
	if (stat & DW_IC_INTR_TX_ABRT) {
		/* The source register clears with IC_CLR_TX_ABRT. */
		dev->abort_source = dw_readl(dev, DW_IC_TX_ABRT_SOURCE);
		(void)dw_readl(dev, DW_IC_CLR_TX_ABRT);
	}
	if (stat & DW_IC_INTR_RX_DONE)
		(void)dw_readl(dev, DW_IC_CLR_RX_DONE);
	if (stat & DW_IC_INTR_ACTIVITY)
		(void)dw_readl(dev, DW_IC_CLR_ACTIVITY);
	/* STOP_DET stays set while received bytes are still on their way,
	 * so it is seen again together with them. */
	if ((stat & DW_IC_INTR_STOP_DET) &&
	    (dev->rx_outstanding == 0 || (stat & DW_IC_INTR_RX_FULL)))
		(void)dw_readl(dev, DW_IC_CLR_STOP_DET);
	if (stat & DW_IC_INTR_START_DET)
		(void)dw_readl(dev, DW_IC_CLR_START_DET);
	if (stat & DW_IC_INTR_GEN_CALL)
		(void)dw_readl(dev, DW_IC_CLR_GEN_CALL);
	return stat;
}

/* ==========================================================================
 * The state machine
 * ========================================================================== */

/* Fill the TX FIFO from the messages. */
static void i2c_dw_xfer_msg(struct dw_i2c_dev *dev)
{
	struct i2c_msg *msgs = dev->msgs;
	uint32_t intr_mask = DW_IC_INTR_MASTER_MASK;
	uint32_t buf_len = dev->tx_buf_len;
	uint8_t *buf = dev->tx_buf;
	int tx_limit, rx_limit, rx_tl;

	for (; dev->msg_write_idx < dev->msgs_num; dev->msg_write_idx++) {
		uint32_t flags = msgs[dev->msg_write_idx].flags;

		if (!(dev->status & DW_STATUS_WRITE_IN_PROGRESS)) {
			/* a new message */
			buf = msgs[dev->msg_write_idx].buf;
			buf_len = msgs[dev->msg_write_idx].len;
			/* With both "hold the bus on an empty FIFO" and
			 * RESTART enabled, the RESTART between messages is
			 * the driver's to set.  Kept in the status until the
			 * message's first command is written: a message that
			 * starts on a full FIFO (or at the RX limit) is
			 * continued by a later call, which must still set
			 * it -- the controller adds a RESTART by itself only
			 * on a change of direction. */
			if ((dev->master_cfg & DW_IC_CON_RESTART_EN) &&
			    dev->msg_write_idx > 0)
				dev->status |= DW_STATUS_NEED_RESTART;
		}

		tx_limit = (int)dev->tx_fifo_depth -
			   (int)dw_readl(dev, DW_IC_TXFLR);
		rx_limit = (int)dev->rx_fifo_depth -
			   (int)dw_readl(dev, DW_IC_RXFLR);

		while (buf_len > 0 && tx_limit > 0 && rx_limit > 0) {
			uint32_t cmd = 0;

			/* STOP with the last byte of the last message
			 * (whether the controller would stop on its own on
			 * an empty FIFO cannot be read from it).  Not for a
			 * received-length read: its length is not known
			 * yet. */
			if (dev->msg_write_idx == dev->msgs_num - 1 &&
			    buf_len == 1 && !(flags & I2C_M_RECV_LEN))
				cmd |= DW_IC_DATA_CMD_STOP;
			if (dev->status & DW_STATUS_NEED_RESTART)
				cmd |= DW_IC_DATA_CMD_RESTART;

			if (flags & I2C_M_RD) {
				/* no RX FIFO overrun */
				if (dev->rx_outstanding >=
				    (int)dev->rx_fifo_depth)
					break;
				/* a block read queues only its length byte
				 * until i2c_dw_recv_len() ran */
				if ((flags & I2C_M_RECV_LEN) &&
				    buf_len < msgs[dev->msg_write_idx].len)
					break;
				dw_writel(dev, cmd | DW_IC_DATA_CMD_READ,
					  DW_IC_DATA_CMD);
				rx_limit--;
				dev->rx_outstanding++;
			} else {
				dw_writel(dev, cmd | *buf++, DW_IC_DATA_CMD);
			}
			dev->status &= ~DW_STATUS_NEED_RESTART;
			tx_limit--;
			buf_len--;
		}

		dev->tx_buf = buf;
		dev->tx_buf_len = buf_len;

		/* A received-length read cannot go on until its length
		 * is in; TX_EMPTY would only flood meanwhile. */
		if (flags & I2C_M_RECV_LEN) {
			dev->status |= DW_STATUS_WRITE_IN_PROGRESS;
			intr_mask &= ~DW_IC_INTR_TX_EMPTY;
			break;
		} else if (buf_len > 0) {
			/* more to write */
			dev->status |= DW_STATUS_WRITE_IN_PROGRESS;
			break;
		} else {
			dev->status &= ~DW_STATUS_WRITE_IN_PROGRESS;
		}
	}

	/* Every message queued: no TX_EMPTY any more. */
	if (dev->msg_write_idx == dev->msgs_num)
		intr_mask &= ~DW_IC_INTR_TX_EMPTY;
	if (dev->msg_err)
		intr_mask = 0;

	/* One RX_FULL for the burst of reads queued; capped while more is
	 * to be queued so the FIFO is drained in time for the next
	 * TX_EMPTY to queue the rest. */
	rx_tl = dev->rx_outstanding;
	if (dev->msg_write_idx < dev->msgs_num &&
	    rx_tl > (int)dev->rx_fifo_depth / 2)
		rx_tl = (int)dev->rx_fifo_depth / 2;
	dw_writel(dev, rx_tl ? (uint32_t)(rx_tl - 1) : 0, DW_IC_RX_TL);

	dw_write_intr_mask(dev, intr_mask);
}

/* The length byte of a received-length read arrived. */
static uint8_t i2c_dw_recv_len(struct dw_i2c_dev *dev, uint8_t len)
{
	struct i2c_msg *msgs = dev->msgs;
	uint32_t flags = msgs[dev->msg_read_idx].flags;
	uint32_t intr_mask;
	uint32_t total;

	/* the length byte itself, and a PEC byte when asked for */
	total = (uint32_t)len + ((flags & I2C_CLIENT_PEC) ? 2u : 1u);
	dev->tx_buf_len = total - ((uint32_t)dev->rx_outstanding < total ?
					   (uint32_t)dev->rx_outstanding :
					   total);
	msgs[dev->msg_read_idx].len = (uint16_t)total;
	msgs[dev->msg_read_idx].flags &= (uint16_t)~I2C_M_RECV_LEN;

	/* The length is known: queueing goes on. */
	intr_mask = dw_read_intr_mask(dev);
	intr_mask |= DW_IC_INTR_TX_EMPTY;
	dw_write_intr_mask(dev, intr_mask);
	return (uint8_t)total;
}

/* Drain the RX FIFO into the read messages. */
static void i2c_dw_read(struct dw_i2c_dev *dev)
{
	struct i2c_msg *msgs = dev->msgs;

	for (; dev->msg_read_idx < dev->msgs_num; dev->msg_read_idx++) {
		uint32_t flags = msgs[dev->msg_read_idx].flags;
		uint32_t len, rx_valid, tmp;
		uint8_t *buf;

		if (!(flags & I2C_M_RD))
			continue;

		if (!(dev->status & DW_STATUS_READ_IN_PROGRESS)) {
			len = msgs[dev->msg_read_idx].len;
			buf = msgs[dev->msg_read_idx].buf;
		} else {
			len = dev->rx_buf_len;
			buf = dev->rx_buf;
		}

		rx_valid = dw_readl(dev, DW_IC_RXFLR);
		for (; len > 0 && rx_valid > 0; len--, rx_valid--) {
			tmp = dw_readl(dev, DW_IC_DATA_CMD) &
			      DW_IC_DATA_CMD_DAT;
			if (flags & I2C_M_RECV_LEN) {
				/* An invalid length still needs one more
				 * byte read with STOP to end the
				 * transaction: it becomes 1. */
				if (!tmp || tmp > I2C_SMBUS_BLOCK_MAX)
					tmp = 1;
				len = i2c_dw_recv_len(dev, (uint8_t)tmp);
				flags &= ~(uint32_t)I2C_M_RECV_LEN;
			}
			*buf++ = (uint8_t)tmp;
			dev->rx_outstanding--;
		}

		if (len > 0) {
			dev->status |= DW_STATUS_READ_IN_PROGRESS;
			dev->rx_buf_len = len;
			dev->rx_buf = buf;
			return;
		}
		dev->status &= ~DW_STATUS_READ_IN_PROGRESS;
	}
}

static void i2c_dw_process_transfer(struct dw_i2c_dev *dev, uint32_t stat)
{
	if (stat & DW_IC_INTR_TX_ABRT) {
		dev->cmd_err |= DW_IC_ERR_TX_ABRT;
		dev->status &= ~DW_STATUS_MASK;
		dev->rx_outstanding = 0;
		/* The FIFOs are flushed with an abort: nothing more to
		 * read or write. */
		dw_write_intr_mask(dev, 0);
		goto tx_aborted;
	}

	if (stat & DW_IC_INTR_RX_FULL)
		i2c_dw_read(dev);
	if (stat & DW_IC_INTR_TX_EMPTY)
		i2c_dw_xfer_msg(dev);

	/* A STOP in the middle of a read or a write. */
	if ((stat & DW_IC_INTR_STOP_DET) &&
	    (dev->status & (DW_STATUS_READ_IN_PROGRESS | DW_STATUS_WRITE_IN_PROGRESS))) {
		dev->st.spurious_stop++;
		dev->rx_outstanding = 0;
		dev->msg_err = -EIO;
	}

tx_aborted:
	if (((stat & (DW_IC_INTR_TX_ABRT | DW_IC_INTR_STOP_DET)) ||
	     dev->msg_err) &&
	    dev->rx_outstanding == 0) {
		complete(&dev->cmd_complete);
	} else if (dev->flags & ACCESS_INTR_MASK) {
		/* Writing the mask again makes a pending interrupt raise
		 * the line anew. */
		uint32_t m = dw_read_intr_mask(dev);

		dw_write_intr_mask(dev, 0);
		dw_write_intr_mask(dev, m);
	}
}

int i2c_dw_isr_master(struct dw_i2c_dev *dev)
{
	uint32_t stat, enabled;

	__atomic_add_fetch(&dev->st.irqs, 1, __ATOMIC_RELAXED);
	/* Powered down: the registers are not to be touched, and the
	 * interrupt cannot be ours. */
	if (__atomic_load_n(&dev->rpm_suspended, __ATOMIC_ACQUIRE))
		goto none;
	enabled = dw_readl(dev, DW_IC_ENABLE);
	stat = dw_readl(dev, DW_IC_RAW_INTR_STAT);
	if (!enabled || !(stat & ~DW_IC_INTR_ACTIVITY))
		goto none;
	/* all ones: the controller is gone (or in D3) */
	if (stat == 0xffffffffu)
		goto none;

	spin_lock(&dev->lock);
	if (dev->polling_now) {
		/* A polled part owns the status bits. */
		spin_unlock(&dev->lock);
		goto none;
	}
	stat = i2c_dw_read_clear_intrbits(dev);
	if (!(dev->status & DW_STATUS_ACTIVE)) {
		/* No transfer: the state is stale.  Acknowledged and
		 * masked, in case firmware left the controller active. */
		dw_write_intr_mask(dev, 0);
		spin_unlock(&dev->lock);
		__atomic_add_fetch(&dev->st.irq_unexpected, 1,
				   __ATOMIC_RELAXED);
		return IRQ_HANDLED;
	}
	i2c_dw_process_transfer(dev, stat);
	spin_unlock(&dev->lock);
	return IRQ_HANDLED;
none:
	__atomic_add_fetch(&dev->st.irq_none, 1, __ATOMIC_RELAXED);
	return IRQ_NONE;
}

/* Program the target and start (the controller disabled before). */
static void i2c_dw_xfer_init(struct dw_i2c_dev *dev)
{
	struct i2c_msg *msgs = dev->msgs;
	uint32_t ic_con = 0, ic_tar = 0;
	uint64_t fl;

	/* Outside dev->lock: disabling may sleep.  No transfer is active
	 * (DW_STATUS_ACTIVE clear), so the handler keeps its hands off. */
	__i2c_dw_disable(dev);

	spin_lock_irqsave(&dev->lock, &fl);
	if (msgs[dev->msg_write_idx].flags & I2C_M_TEN) {
		ic_con = DW_IC_CON_10BITADDR_MASTER;
		/* With dynamic TAR updates 10-bit addressing is enabled in
		 * IC_TAR; that cannot be detected, so it is always set. */
		ic_tar = DW_IC_TAR_10BITADDR_MASTER;
	}
	dw_update_bits(dev, DW_IC_CON, DW_IC_CON_10BITADDR_MASTER, ic_con);
	dw_writel(dev, msgs[dev->msg_write_idx].addr | ic_tar, DW_IC_TAR);

	/* Interrupts off while enabling (hardware issues). */
	dw_write_intr_mask(dev, 0);
	dw_enable(dev);
	/* A dummy read keeps the register from getting stuck (Bay
	 * Trail). */
	(void)dw_readl(dev, DW_IC_ENABLE_STATUS);
	/* Clear, then unmask: the first TX_EMPTY starts the transfer. */
	(void)dw_readl(dev, DW_IC_CLR_INTR);
	dw_write_intr_mask(dev, DW_IC_INTR_MASTER_MASK);
	spin_unlock_irqrestore(&dev->lock, fl);
}

/* Is the controller still active after a STOP (it can hold SCL low when
 * disabled at the wrong moment)?  Waits up to 20 ms for it to go idle. */
static int i2c_dw_is_controller_active(struct dw_i2c_dev *dev)
{
	uint64_t deadline;
	uint32_t status;

	status = dw_readl(dev, DW_IC_STATUS);
	if (!(status & DW_IC_STATUS_MASTER_ACTIVITY))
		return 0;
	deadline = hrtimer_now_ns() + 20000ull * 1000ull;
	while (hrtimer_now_ns() < deadline) {
		i2c_dw_delay(dev, 276, 1100);
		status = dw_readl(dev, DW_IC_STATUS);
		if (!(status & DW_IC_STATUS_MASTER_ACTIVITY))
			return 0;
	}
	return 1;
}

static int i2c_dw_wait_transfer(struct dw_i2c_dev *dev)
{
	long timeout_ms = dev->adapter.timeout;
	uint64_t deadline;
	int done = 0;

	if (!dev->polling_now) {
		if (wait_for_completion_timeout(&dev->cmd_complete,
						timeout_ms > 0 ? timeout_ms : 1))
			return 0;
		return -ETIMEDOUT;
	}

	deadline = hrtimer_now_ns() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0) *
					      1000000ull;
	do {
		uint32_t stat;
		uint64_t fl;

		if (try_wait_for_completion(&dev->cmd_complete)) {
			done = 1;
			break;
		}
		spin_lock_irqsave(&dev->lock, &fl);
		stat = i2c_dw_read_clear_intrbits(dev);
		if (stat)
			i2c_dw_process_transfer(dev, stat);
		spin_unlock_irqrestore(&dev->lock, fl);
		if (!stat)
			/* nothing yet: save some power */
			i2c_dw_delay(dev, 3, 25);
	} while (hrtimer_now_ns() < deadline);
	if (!done)
		done = try_wait_for_completion(&dev->cmd_complete);
	return done ? 0 : -ETIMEDOUT;
}

/* One part: start, wait, end.  0 or -errno. */
static int __i2c_dw_xfer_one_part(struct dw_i2c_dev *dev, struct i2c_msg *msgs,
				  int num)
{
	int msg_err, cmd_err, ret;
	unsigned int status;
	uint64_t fl;

	reinit_completion(&dev->cmd_complete);
	spin_lock_irqsave(&dev->lock, &fl);
	dev->msgs = msgs;
	dev->msgs_num = num;
	dev->cmd_err = 0;
	dev->msg_write_idx = 0;
	dev->msg_read_idx = 0;
	dev->msg_err = 0;
	dev->status = 0;
	dev->abort_source = 0;
	dev->rx_outstanding = 0;
	dev->tx_buf_len = 0;
	dev->tx_buf = NULL;
	dev->rx_buf_len = 0;
	dev->rx_buf = NULL;
	dev->polling_now = dev->atomic_now || (dev->flags & ACCESS_POLLING);
	spin_unlock_irqrestore(&dev->lock, fl);
	i2c_stat_add(&dev->st.xfers, 1);
	if (dev->polling_now)
		i2c_stat_add(&dev->st.polled, 1);

	ret = i2c_dw_wait_bus_not_busy(dev);
	if (ret < 0)
		goto out;

	i2c_dw_xfer_init(dev);

	ret = i2c_dw_wait_transfer(dev);

	/* The part is over for the handler: nothing of the messages is
	 * touched after this. */
	spin_lock_irqsave(&dev->lock, &fl);
	dw_write_intr_mask(dev, 0);
	status = dev->status & ~DW_STATUS_ACTIVE;
	dev->status &= ~DW_STATUS_ACTIVE;
	msg_err = dev->msg_err;
	cmd_err = dev->cmd_err;
	dev->msgs = NULL;
	spin_unlock_irqrestore(&dev->lock, fl);

	if (ret) {
		unsigned int missed = 0;

		/* The ladder: the controller re-initialised (which disables
		 * it as well), the bus recovered; a client that keeps timing
		 * out resets its device (its driver's business). */
		i2c_stat_add(&dev->st.timeouts, 1);
		if (i2c_ratelimit_ok(&dev->rl, &missed))
			kprintf("%s: controller timed out%s\n", dev->name,
				missed ? " (earlier messages suppressed)" : "");
		i2c_recover_bus(&dev->adapter);
		i2c_stat_add(&dev->st.reinits, 1);
		(void)i2c_dw_init(dev);
		goto out;
	}

	/* Rarely the controller is still active at STOP_DET; disabling it
	 * then can leave SCL held low.  Wait for it to go idle. */
	if (i2c_dw_is_controller_active(dev)) {
		unsigned int missed = 0;

		i2c_stat_add(&dev->st.active_after, 1);
		if (i2c_ratelimit_ok(&dev->rl, &missed))
			kprintf("%s: controller active after xfer, recovering%s\n",
				dev->name,
				missed ? " (earlier messages suppressed)" : "");
		i2c_recover_bus(&dev->adapter);
		i2c_stat_add(&dev->st.reinits, 1);
		(void)i2c_dw_init(dev);
	} else {
		/* Disabled before the next transfer so it cannot raise
		 * interrupts in between. */
		dw_disable_nowait(dev);
	}

	if (msg_err) {
		ret = msg_err;
		goto out;
	}
	if (!cmd_err && !status) {
		ret = 0;
		goto out;
	}
	if (cmd_err == DW_IC_ERR_TX_ABRT) {
		ret = i2c_dw_handle_tx_abort(dev);
		goto out;
	}
	if (status) {
		i2c_stat_add(&dev->st.early_term, 1);
		kprintf("%s: transfer terminated early - interrupt latency too high?\n",
			dev->name);
	}
	ret = -EIO;
out:
	spin_lock_irqsave(&dev->lock, &fl);
	dev->polling_now = 0;
	dev->msgs = NULL;
	spin_unlock_irqrestore(&dev->lock, fl);
	return ret;
}

/* May message `idx' follow the one before it in the same transaction? */
static int i2c_dw_msg_is_valid(struct dw_i2c_dev *dev,
			       const struct i2c_msg *msgs, int idx)
{
	if (!idx)
		return 1;
	/* The target cannot change within a transaction. */
	if (msgs[idx - 1].addr != msgs[idx].addr) {
		kprintf("%s: invalid target address\n", dev->name);
		return 0;
	}
	/* Without "hold on empty FIFO" no RESTART between two messages of
	 * the same direction. */
	if (!dev->emptyfifo_hold_master &&
	    (msgs[idx - 1].flags & I2C_M_RD) == (msgs[idx].flags & I2C_M_RD)) {
		kprintf("%s: cannot emit RESTART\n", dev->name);
		return 0;
	}
	return 1;
}

static int i2c_dw_xfer_common(struct dw_i2c_dev *dev, struct i2c_msg *msgs,
			      int num)
{
	struct i2c_msg *part;
	int ret = 0;

	BUG_ON(!msgs || num < 1);
	i2c_dw_rpm_get(dev);
	ret = i2c_dw_acquire_lock(dev);
	if (ret)
		goto out_rpm;

	/* Messages with I2C_M_STOP end a transaction: one part each. */
	for (part = msgs; part < msgs + num;) {
		int cnt;

		for (cnt = 1;; cnt++) {
			if (!i2c_dw_msg_is_valid(dev, part, cnt - 1)) {
				ret = -EINVAL;
				break;
			}
			if ((part[cnt - 1].flags & I2C_M_STOP) ||
			    part + cnt == msgs + num)
				break;
		}
		if (ret < 0)
			break;
		ret = __i2c_dw_xfer_one_part(dev, part, cnt);
		if (ret < 0)
			break;
		part += cnt;
	}

	i2c_dw_release_lock(dev);
out_rpm:
	i2c_dw_rpm_put(dev);
	return ret < 0 ? ret : num;
}

int i2c_dw_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	struct dw_i2c_dev *dev = i2c_get_adapdata(adap);

	might_sleep();
	dev->atomic_now = 0;
	return i2c_dw_xfer_common(dev, msgs, num);
}

/* With interrupts off (shutdown, panic paths): polled, never sleeping. */
int i2c_dw_xfer_atomic(struct i2c_adapter *adap, struct i2c_msg *msgs,
		       int num)
{
	struct dw_i2c_dev *dev = i2c_get_adapdata(adap);
	int ret;

	if (dev->acquire_lock)
		return -EAGAIN; /* a firmware semaphore may sleep */
	dev->atomic_now = 1;
	ret = i2c_dw_xfer_common(dev, msgs, num);
	dev->atomic_now = 0;
	return ret;
}

/* ==========================================================================
 * Bus recovery
 * ========================================================================== */

static void i2c_dw_prepare_recovery(struct i2c_adapter *adap)
{
	struct dw_i2c_dev *dev = i2c_get_adapdata(adap);

	i2c_dw_disable(dev);
}

static void i2c_dw_unprepare_recovery(struct i2c_adapter *adap)
{
	struct dw_i2c_dev *dev = i2c_get_adapdata(adap);

	(void)i2c_dw_init(dev);
}

/* The controller's own bus clear (v2.02 and later, when firmware enabled
 * the feature): it clocks SCL until the target releases SDA (at most
 * nine pulses) and sends a STOP. */
static int i2c_dw_bus_clear_recover(struct i2c_adapter *adap)
{
	struct dw_i2c_dev *dev = i2c_get_adapdata(adap);
	uint32_t en, status;
	uint64_t deadline;
	int ret = 0;

	i2c_stat_add(&dev->st.hw_bus_clear, 1);
	en = dw_readl(dev, DW_IC_ENABLE);
	if (!(en & DW_IC_ENABLE_ENABLE)) {
		dw_writel(dev, DW_IC_ENABLE_ENABLE, DW_IC_ENABLE);
		i2c_dw_delay(dev, 25, 100);
		en |= DW_IC_ENABLE_ENABLE;
	}
	dw_writel(dev, en | DW_IC_ENABLE_SDA_STUCK_RECOVERY, DW_IC_ENABLE);
	/* The bit clears when the controller is done (nine SCL periods at
	 * the slowest speed are under 100 us; 10 ms is plenty). */
	deadline = hrtimer_now_ns() + 10000000ull;
	while (dw_readl(dev, DW_IC_ENABLE) & DW_IC_ENABLE_SDA_STUCK_RECOVERY) {
		if (hrtimer_now_ns() > deadline) {
			ret = -ETIMEDOUT;
			break;
		}
		i2c_dw_delay(dev, 50, 100);
	}
	status = dw_readl(dev, DW_IC_STATUS);
	if (!ret && (status & DW_IC_STATUS_SDA_STUCK_NOT_RECOVERED))
		ret = -EBUSY;
	(void)i2c_dw_init(dev);
	if (ret)
		kprintf("%s: bus clear failed (%d, status 0x%x)\n", dev->name,
			ret, status);
	return ret;
}

static void i2c_dw_init_recovery_info(struct dw_i2c_dev *dev)
{
	struct i2c_bus_recovery_info *rinfo = &dev->rinfo;
	struct i2c_adapter *adap = &dev->adapter;
	struct gpio_desc *scl = NULL, *sda = NULL;
	int rc;

	kmemset(rinfo, 0, sizeof(*rinfo));
	adap->bus_recovery_info = NULL;

	/* GPIO recovery when firmware describes the lines. */
	adap->acpi_dev = dev->acpi_dev;
	adap->acpi_handle = dev->acpi_dev ? dev->acpi_dev->handle : NULL;
	rc = i2c_acpi_get_recovery_gpios(adap, &scl, &sda);
	if (!rc && scl) {
		rinfo->scl_gpiod = scl;
		rinfo->sda_gpiod = sda;
		rinfo->recover_bus = i2c_generic_scl_recovery;
		rinfo->prepare_recovery = i2c_dw_prepare_recovery;
		rinfo->unprepare_recovery = i2c_dw_unprepare_recovery;
		adap->bus_recovery_info = rinfo;
		kprintf("%s: running with GPIO recovery mode! scl%s\n",
			dev->name, sda ? ",sda" : "");
		return;
	}
	if (rc == -EPROBE_DEFER)
		kprintf("%s: recovery GPIO controller not ready, no GPIO recovery\n",
			dev->name);

	/* Otherwise the controller's bus clear, where firmware enabled
	 * it on a controller that has it. */
	if ((dev->master_cfg & DW_IC_CON_BUS_CLEAR_CTRL) &&
	    dev->comp_version >= DW_IC_BUS_CLEAR_MIN_VERS) {
		rinfo->recover_bus = i2c_dw_bus_clear_recover;
		adap->bus_recovery_info = rinfo;
		dev->hw_bus_clear = 1;
	}
}

void i2c_dw_release_recovery(struct dw_i2c_dev *dev)
{
	if (dev->rinfo.sda_gpiod)
		gpiod_free(dev->rinfo.sda_gpiod);
	if (dev->rinfo.scl_gpiod)
		gpiod_free(dev->rinfo.scl_gpiod);
	dev->rinfo.sda_gpiod = NULL;
	dev->rinfo.scl_gpiod = NULL;
	dev->adapter.bus_recovery_info = NULL;
}

int i2c_dw_probe_master(struct dw_i2c_dev *dev)
{
	uint32_t ic_con;
	int ret;

	ret = i2c_dw_set_timings_master(dev);
	if (ret)
		return ret;

	ret = i2c_dw_acquire_lock(dev);
	if (ret)
		return ret;
	/* Firmware that turned on the bus clear feature (AMD: its SMU
	 * recovers the bus) keeps it on. */
	ic_con = dw_readl(dev, DW_IC_CON);
	i2c_dw_release_lock(dev);
	if (ic_con & DW_IC_CON_BUS_CLEAR_CTRL)
		dev->master_cfg |= DW_IC_CON_BUS_CLEAR_CTRL;

	i2c_dw_init_recovery_info(dev);
	return 0;
}
