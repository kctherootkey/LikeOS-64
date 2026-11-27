/* DesignWare I2C controller and bus model (see model.h).  Register
 * offsets and bits are spelled out here from the controller's register
 * description, independently of the driver's header, so a wrong constant
 * there shows up as a test failure.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include "host/i2c/model.h"

/* registers */
#define M_CON 0x00
#define M_TAR 0x04
#define M_DATA_CMD 0x10
#define M_SS_HCNT 0x14
#define M_SS_LCNT 0x18
#define M_FS_HCNT 0x1c
#define M_FS_LCNT 0x20
#define M_HS_HCNT 0x24
#define M_HS_LCNT 0x28
#define M_INTR_STAT 0x2c
#define M_INTR_MASK 0x30
#define M_RAW_INTR_STAT 0x34
#define M_RX_TL 0x38
#define M_TX_TL 0x3c
#define M_CLR_INTR 0x40
#define M_CLR_RX_UNDER 0x44
#define M_CLR_RX_OVER 0x48
#define M_CLR_TX_OVER 0x4c
#define M_CLR_RD_REQ 0x50
#define M_CLR_TX_ABRT 0x54
#define M_CLR_RX_DONE 0x58
#define M_CLR_ACTIVITY 0x5c
#define M_CLR_STOP_DET 0x60
#define M_CLR_START_DET 0x64
#define M_CLR_GEN_CALL 0x68
#define M_ENABLE 0x6c
#define M_STATUS 0x70
#define M_TXFLR 0x74
#define M_RXFLR 0x78
#define M_SDA_HOLD 0x7c
#define M_TX_ABRT_SOURCE 0x80
#define M_ENABLE_STATUS 0x9c
#define M_FS_SPKLEN 0xa0
#define M_HS_SPKLEN 0xa4
#define M_CLR_RESTART_DET 0xa8
#define M_SMBUS_INTR_MASK 0xcc
#define M_COMP_PARAM_1 0xf4
#define M_COMP_VERSION 0xf8
#define M_COMP_TYPE 0xfc

/* interrupt causes */
#define I_RX_UNDER (1u << 0)
#define I_RX_OVER (1u << 1)
#define I_RX_FULL (1u << 2)
#define I_TX_OVER (1u << 3)
#define I_TX_EMPTY (1u << 4)
#define I_RD_REQ (1u << 5)
#define I_TX_ABRT (1u << 6)
#define I_RX_DONE (1u << 7)
#define I_ACTIVITY (1u << 8)
#define I_STOP_DET (1u << 9)
#define I_START_DET (1u << 10)
#define I_GEN_CALL (1u << 11)
#define I_RESTART_DET (1u << 12)
#define I_MST_ON_HOLD (1u << 13)

#define CMD_READ (1u << 8)
#define CMD_STOP (1u << 9)
#define CMD_RESTART (1u << 10)

#define CON_10BIT_MASTER (1u << 4)
#define CON_RESTART_EN (1u << 5)
#define TAR_10BIT (1u << 12)

#define ABRT_7B_ADDR_NOACK 0
#define ABRT_10ADDR1_NOACK 1
#define ABRT_TXDATA_NOACK 3
#define ABRT_ARB_LOST 12
#define ABRT_USER 16

#define MAX_MODELS 16
static struct dw_model *g_models[MAX_MODELS];

static uint32_t swab32m(uint32_t v)
{
	return (v >> 24) | ((v >> 8) & 0xff00u) | ((v << 8) & 0xff0000u) |
	       (v << 24);
}

uint8_t dwm_crc8(uint8_t crc, uint8_t b)
{
	uint8_t c = crc ^ b;

	for (int i = 0; i < 8; i++)
		c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
	return c;
}

static void logev(struct dw_model *m, uint8_t type, uint16_t val)
{
	if (m->nlog < DWM_LOG) {
		m->log[m->nlog].type = type;
		m->log[m->nlog].val = val;
		m->nlog++;
	}
}

/* ==========================================================================
 * Targets
 * ========================================================================== */

struct dwm_slave *dwm_slave_new(int kind, uint16_t addr, int ten)
{
	struct dwm_slave *s = kcalloc(1, sizeof(*s));

	T_ASSERT(s);
	s->kind = kind;
	s->addr = addr;
	s->ten = ten;
	s->present = 1;
	return s;
}

static int slv_start(struct dwm_slave *s, int read, uint8_t addr_byte,
		     int restart)
{
	if (s->kind == SLV_CB)
		return s->cb_start ? s->cb_start(s, read, restart) : 1;
	s->read_dir = read;
	s->rcount = 0;
	if (!read)
		s->wcount = 0;
	s->crc = dwm_crc8(s->crc, addr_byte);
	return 1;
}

/* 1: ACK, 0: NACK */
static int slv_write(struct dwm_slave *s, uint8_t b)
{
	int n = ++s->wcount;

	s->bytes_in++;
	if (s->nack_after && n >= s->nack_after)
		return 0;
	if (s->kind == SLV_CB)
		return s->cb_write ? s->cb_write(s, b) : 1;
	s->crc = dwm_crc8(s->crc, b);
	switch (s->kind) {
	case SLV_REGMEM:
		if (n == 1)
			s->ptr = b;
		else
			s->mem[s->ptr++] = b;
		break;
	case SLV_HID:
		if (n == 1)
			s->reg = b;
		else if (n == 2)
			s->reg |= (uint16_t)(b << 8);
		else
			s->mem[(s->reg + n - 3) & 0x1ff] = b;
		break;
	case SLV_SMBUS:
		if (n == 1) {
			s->cmd = b;
		} else if (s->cmd == 0x10 || s->cmd == 0x11) {
			/* block write: count, data, [PEC] */
			if (n == 2)
				s->block_len = b > 32 ? 32 : b;
			else if (n - 3 < s->block_len)
				s->block[n - 3] = b;
		} else {
			s->mem[(uint8_t)(s->cmd + n - 2)] = b;
		}
		break;
	}
	return 1;
}

static uint8_t slv_read(struct dwm_slave *s)
{
	int n = s->rcount++;
	uint8_t b = 0;

	s->bytes_out++;
	if (s->kind == SLV_CB)
		return s->cb_read ? s->cb_read(s) : 0xff;
	switch (s->kind) {
	case SLV_REGMEM:
		b = s->mem[s->ptr++];
		break;
	case SLV_HID:
		b = s->mem[(s->reg + n) & 0x1ff];
		break;
	case SLV_SMBUS:
		if (s->cmd == 0x10 || s->cmd == 0x11) {
			int count = s->bad_count ? s->bad_count : s->block_len;

			if (n == 0)
				b = (uint8_t)count;
			else if (n <= count && n - 1 < 32)
				b = s->block[n - 1];
			else
				b = (uint8_t)(s->crc ^ (s->corrupt_pec ? 0x5a : 0));
		} else if (s->cmd == 0x20) {
			/* a word, then PEC */
			b = n == 0 ? 0x34 : n == 1 ? 0x12 : s->crc;
		} else {
			if (s->pec && n >= 1)
				b = s->crc;
			else
				b = s->mem[(uint8_t)(s->cmd + n)];
		}
		break;
	}
	s->crc = dwm_crc8(s->crc, b);
	return b;
}

static void slv_stop(struct dwm_slave *s)
{
	if (s->kind == SLV_CB && s->cb_stop)
		s->cb_stop(s);
	s->txns++;
	s->crc_at_stop = s->crc;
	s->crc = 0;
}

/* ==========================================================================
 * Controller
 * ========================================================================== */

static int in_reset(struct dw_model *m)
{
	return m->lpss && (m->priv[0x04 / 4] & 3) != 3;
}

static uint32_t raw_now(struct dw_model *m)
{
	uint32_t r = m->raw_latched;

	if (m->enable & 1) {
		if (m->tx_count <= m->tx_tl)
			r |= I_TX_EMPTY;
		if (m->hold)
			r |= I_MST_ON_HOLD;
	}
	if (m->rx_count > m->rx_tl)
		r |= I_RX_FULL;
	return r;
}

int dwm_irq_line(struct dw_model *m)
{
	int line;
	uint64_t fl;

	spin_lock_irqsave(&m->lock, &fl);
	line = !m->gone && !in_reset(m) && (raw_now(m) & m->intr_mask) != 0;
	spin_unlock_irqrestore(&m->lock, fl);
	return line;
}

static void flush_fifos(struct dw_model *m)
{
	m->tx_count = 0;
	m->tx_head = 0;
	m->rx_count = 0;
	m->rx_head = 0;
}

static void end_txn(struct dw_model *m, int stop_det)
{
	if (m->in_txn) {
		if (m->cur)
			slv_stop(m->cur);
		logev(m, EV_STOP, 0);
		m->stops++;
		if (stop_det)
			m->raw_latched |= I_STOP_DET;
	}
	m->in_txn = 0;
	m->cur = NULL;
	m->hold = 0;
	m->stretched = 0;
}

static void do_abort(struct dw_model *m, int src, int send_stop)
{
	m->abrt_source |= 1u << src;
	m->raw_latched |= I_TX_ABRT;
	m->abrt_hold = 1;
	m->aborts++;
	logev(m, EV_ABORT, (uint16_t)src);
	flush_fifos(m);
	if (send_stop) {
		end_txn(m, 1);
	} else {
		m->in_txn = 0;
		m->cur = NULL;
		m->hold = 0;
	}
}

static uint32_t status_now(struct dw_model *m)
{
	uint32_t s = 0;

	if (m->in_txn || m->stuck_sda || ((m->enable & 1) && m->tx_count))
		s |= 1u << 0; /* ACTIVITY */
	if (m->tx_count < m->tx_depth)
		s |= 1u << 1; /* TFNF */
	if (!m->tx_count)
		s |= 1u << 2; /* TFE */
	if (m->rx_count)
		s |= 1u << 3; /* RFNE */
	if (m->rx_count >= m->rx_depth)
		s |= 1u << 4; /* RFF */
	if (m->in_txn)
		s |= 1u << 5; /* MASTER_ACTIVITY */
	if (m->hold)
		s |= 1u << 7;
	if (m->sda_not_recovered)
		s |= 1u << 11;
	return s;
}

static uint32_t reg_read(struct dw_model *m, unsigned int off)
{
	uint32_t v = 0;

	switch (off) {
	case M_CON:
		return m->con;
	case M_TAR:
		return m->tar;
	case M_DATA_CMD:
		if (!m->rx_count) {
			m->raw_latched |= I_RX_UNDER;
			m->rx_under++;
			return 0;
		}
		v = m->rx[m->rx_head];
		m->rx_head = (m->rx_head + 1) % 256;
		m->rx_count--;
		return v;
	case M_SS_HCNT:
		return m->ss_hcnt;
	case M_SS_LCNT:
		return m->ss_lcnt;
	case M_FS_HCNT:
		return m->fs_hcnt;
	case M_FS_LCNT:
		return m->fs_lcnt;
	case M_HS_HCNT:
		return m->hs_hcnt;
	case M_HS_LCNT:
		return m->hs_lcnt;
	case M_INTR_STAT:
		return raw_now(m) & m->intr_mask;
	case M_INTR_MASK:
		return m->intr_mask;
	case M_RAW_INTR_STAT:
		return raw_now(m);
	case M_RX_TL:
		return m->rx_tl;
	case M_TX_TL:
		return m->tx_tl;
	case M_CLR_INTR:
		m->raw_latched = 0;
		m->abrt_source = 0;
		m->abrt_hold = 0;
		return 0;
	case M_CLR_RX_UNDER:
		m->raw_latched &= ~I_RX_UNDER;
		return 0;
	case M_CLR_RX_OVER:
		m->raw_latched &= ~I_RX_OVER;
		return 0;
	case M_CLR_TX_OVER:
		m->raw_latched &= ~I_TX_OVER;
		return 0;
	case M_CLR_RD_REQ:
		m->raw_latched &= ~I_RD_REQ;
		return 0;
	case M_CLR_TX_ABRT:
		m->raw_latched &= ~I_TX_ABRT;
		m->abrt_source = 0;
		m->abrt_hold = 0;
		return 0;
	case M_CLR_RX_DONE:
		m->raw_latched &= ~I_RX_DONE;
		return 0;
	case M_CLR_ACTIVITY:
		m->raw_latched &= ~I_ACTIVITY;
		return 0;
	case M_CLR_STOP_DET:
		m->raw_latched &= ~I_STOP_DET;
		return 0;
	case M_CLR_START_DET:
		m->raw_latched &= ~I_START_DET;
		return 0;
	case M_CLR_GEN_CALL:
		m->raw_latched &= ~I_GEN_CALL;
		return 0;
	case M_CLR_RESTART_DET:
		m->raw_latched &= ~I_RESTART_DET;
		return 0;
	case M_ENABLE:
		return m->enable;
	case M_STATUS:
		return status_now(m);
	case M_TXFLR:
		return m->tx_count;
	case M_RXFLR:
		return m->rx_count;
	case M_SDA_HOLD:
		return m->sda_hold;
	case M_TX_ABRT_SOURCE:
		return m->abrt_source;
	case M_ENABLE_STATUS:
		if (m->enable & 1)
			return 1;
		if (m->enable_status_count > 0) {
			m->enable_status_count--;
			return 1;
		}
		return 0;
	case M_FS_SPKLEN:
		return m->fs_spklen;
	case M_HS_SPKLEN:
		return m->hs_spklen;
	case M_SMBUS_INTR_MASK:
		return m->smbus_intr_mask;
	case M_COMP_PARAM_1:
		return m->param1;
	case M_COMP_VERSION:
		return m->version;
	case M_COMP_TYPE:
		return m->comp_type;
	default:
		return 0;
	}
}

static void reg_write(struct dw_model *m, unsigned int off, uint32_t v)
{
	int en = m->enable & 1;

	switch (off) {
	case M_CON:
		if (!en)
			m->con = v & 0xfff;
		break;
	case M_TAR:
		if (!en)
			m->tar = v & 0x1fff;
		break;
	case M_DATA_CMD:
		if (!en || m->abrt_hold)
			break; /* held flushed */
		if (m->tx_count >= m->tx_depth) {
			m->raw_latched |= I_TX_OVER;
			m->tx_over++;
			break;
		}
		m->tx[(m->tx_head + m->tx_count) % 256] = v & 0x7ff;
		m->tx_count++;
		m->cmds++;
		if (m->tx_count > m->max_txflr)
			m->max_txflr = m->tx_count;
		break;
	case M_SS_HCNT:
		if (!en)
			m->ss_hcnt = v & 0xffff;
		break;
	case M_SS_LCNT:
		if (!en)
			m->ss_lcnt = v & 0xffff;
		break;
	case M_FS_HCNT:
		if (!en)
			m->fs_hcnt = v & 0xffff;
		break;
	case M_FS_LCNT:
		if (!en)
			m->fs_lcnt = v & 0xffff;
		break;
	case M_HS_HCNT:
		if (!en)
			m->hs_hcnt = v & 0xffff;
		break;
	case M_HS_LCNT:
		if (!en)
			m->hs_lcnt = v & 0xffff;
		break;
	case M_INTR_MASK:
		m->intr_mask = v & 0x7fff;
		break;
	case M_RX_TL:
		m->rx_tl = v < m->rx_depth ? v : m->rx_depth - 1;
		break;
	case M_TX_TL:
		m->tx_tl = v < m->tx_depth ? v : m->tx_depth - 1;
		break;
	case M_SDA_HOLD:
		if (!en)
			m->sda_hold = v;
		break;
	case M_FS_SPKLEN:
		if (!en)
			m->fs_spklen = v ? v & 0xff : 1;
		break;
	case M_HS_SPKLEN:
		if (!en)
			m->hs_spklen = v ? v & 0xff : 1;
		break;
	case M_SMBUS_INTR_MASK:
		m->smbus_intr_mask = v;
		break;
	case M_ENABLE:
		if ((v & 2) && en) {
			/* abort: the transfer ends, STOP on the bus */
			do_abort(m, ABRT_USER, 1);
		}
		if (v & 8) {
			if (m->bus_clear_hw && (en || (v & 1)))
				m->recovery_pending = 1;
		}
		if ((v & 1) && !en) {
			m->enable = 1;
			m->enable_status_count = 0;
		} else if (!(v & 1) && en) {
			if (m->in_txn)
				m->disabled_while_active++;
			m->enable = 0;
			flush_fifos(m);
			end_txn(m, 0);
			m->raw_latched = 0;
			m->abrt_hold = 0;
			m->enable_status_count = m->disable_latency;
		}
		m->enable = (m->enable & 1) | (m->recovery_pending ? 8 : 0);
		break;
	default:
		break;
	}
}

static struct dw_model *find_model(const volatile void *a, unsigned int *off)
{
	unsigned long p = (unsigned long)a;

	for (int i = 0; i < MAX_MODELS; i++) {
		struct dw_model *m = g_models[i];

		if (m && p >= (unsigned long)m->window &&
		    p < (unsigned long)m->window + sizeof(m->window)) {
			*off = (unsigned int)(p - (unsigned long)m->window);
			return m;
		}
	}
	test_bug("MMIO access outside every model", __FILE__, __LINE__,
		 __func__);
}

/* A 32-bit access at `off' (the register file or the LPSS block). */
static uint32_t access_read(struct dw_model *m, unsigned int off)
{
	if (m->gone)
		return 0xffffffffu;
	if (m->lpss && off >= 0x200 && off < 0x300)
		return m->priv[(off - 0x200) / 4];
	if (off >= 0x100 || in_reset(m))
		return 0;
	return reg_read(m, off);
}

static void access_write(struct dw_model *m, unsigned int off, uint32_t v)
{
	if (m->gone)
		return;
	if (m->lpss && off >= 0x200 && off < 0x300) {
		if (off == 0x2fc)
			return; /* capabilities are read-only */
		m->priv[(off - 0x200) / 4] = v;
		return;
	}
	if (off >= 0x100 || in_reset(m))
		return;
	reg_write(m, off, v);
}

uint32_t model_readl(const volatile void *a)
{
	unsigned int off;
	struct dw_model *m = find_model(a, &off);
	uint32_t v;
	uint64_t fl;

	T_ASSERT(!(off & 3));
	spin_lock_irqsave(&m->lock, &fl);
	if (m->gone) {
		v = 0xffffffffu;
	} else if (m->lpss && off >= 0x200 && off < 0x300) {
		v = access_read(m, off);
	} else if (m->access == DWM_ACC_SWAP) {
		v = swab32m(access_read(m, off));
	} else if (m->access == DWM_ACC_16) {
		/* a 16-bit bus: only the low half arrives */
		v = access_read(m, off) & 0xffffu;
	} else {
		v = access_read(m, off);
	}
	spin_unlock_irqrestore(&m->lock, fl);
	return v;
}

void model_writel(uint32_t v, volatile void *a)
{
	unsigned int off;
	struct dw_model *m = find_model(a, &off);
	uint64_t fl;

	T_ASSERT(!(off & 3));
	spin_lock_irqsave(&m->lock, &fl);
	if (m->lpss && off >= 0x200 && off < 0x300)
		access_write(m, off, v);
	else if (m->access == DWM_ACC_SWAP)
		access_write(m, off, swab32m(v));
	else if (m->access == DWM_ACC_16)
		access_write(m, off, v & 0xffffu); /* the high half is lost */
	else
		access_write(m, off, v);
	spin_unlock_irqrestore(&m->lock, fl);
}

uint16_t model_readw(const volatile void *a)
{
	unsigned int off;
	struct dw_model *m = find_model(a, &off);
	uint16_t v;
	uint64_t fl;

	T_ASSERT(!(off & 1));
	T_ASSERT(m->access == DWM_ACC_16);
	spin_lock_irqsave(&m->lock, &fl);
	if (!(off & 2)) {
		/* the low half carries the side effects of the read */
		uint32_t full = access_read(m, off);

		m->r16_cache[(off / 4) & 63] = full;
		v = (uint16_t)full;
	} else {
		v = (uint16_t)(m->r16_cache[(off / 4) & 63] >> 16);
	}
	spin_unlock_irqrestore(&m->lock, fl);
	return v;
}

void model_writew(uint16_t v, volatile void *a)
{
	unsigned int off;
	struct dw_model *m = find_model(a, &off);
	uint64_t fl;

	T_ASSERT(!(off & 1));
	T_ASSERT(m->access == DWM_ACC_16);
	spin_lock_irqsave(&m->lock, &fl);
	if (!(off & 2))
		m->w16_stash[(off / 4) & 63] = v;
	else
		access_write(m, off & ~3u,
			     m->w16_stash[(off / 4) & 63] | ((uint32_t)v << 16));
	spin_unlock_irqrestore(&m->lock, fl);
}

/* ==========================================================================
 * The bus
 * ========================================================================== */

static struct dwm_slave *find_slave(struct dw_model *m, uint16_t addr, int ten)
{
	for (int i = 0; i < m->nslaves; i++) {
		struct dwm_slave *s = m->slaves[i];

		if (s->present && s->addr == addr && s->ten == ten)
			return s;
	}
	return NULL;
}

/* START or RESTART with the address phase: 1 ok, 0 aborted. */
static int address_phase(struct dw_model *m, int read, int restart)
{
	uint16_t addr = m->tar & 0x3ff;
	int ten = (m->con & CON_10BIT_MASTER) || (m->tar & TAR_10BIT);
	struct dwm_slave *s;

	if (!ten)
		addr &= 0x7f;
	logev(m, restart ? EV_RESTART : EV_START, 0);
	if (m->on_start)
		m->on_start(m);
	if (!restart) {
		m->starts++;
		if (m->arb_lose > 0) {
			m->arb_lose--;
			logev(m, EV_ARB_LOST, 0);
			do_abort(m, ABRT_ARB_LOST, 0);
			return 0;
		}
	}
	m->in_txn = 1;
	logev(m, EV_ADDR,
	      (uint16_t)(addr | (read ? 0x8000 : 0) | (ten ? 0x4000 : 0)));
	s = find_slave(m, addr, ten);
	if (!s || !slv_start(s, read, (uint8_t)((addr << 1) | read), restart)) {
		logev(m, EV_NACK_ADDR, 0);
		do_abort(m, ten ? ABRT_10ADDR1_NOACK : ABRT_7B_ADDR_NOACK, 1);
		return 0;
	}
	m->cur = s;
	m->cur_read = read;
	return 1;
}

/* One step: 0 idle, else the microseconds of clock stretching due. */
static unsigned int dwm_step(struct dw_model *m)
{
	uint32_t cmd;
	int read;
	uint64_t fl;

	spin_lock_irqsave(&m->lock, &fl);
	if (!(m->enable & 1) || m->gone || in_reset(m)) {
		if (m->recovery_pending && !(m->enable & 1)) {
			m->recovery_pending = 0;
			m->enable &= ~8u;
		}
		spin_unlock_irqrestore(&m->lock, fl);
		return 0;
	}
	if (m->recovery_pending) {
		/* the bus clear: up to nine SCL pulses, then STOP */
		logev(m, EV_RECOVER, 0);
		if (m->stuck_sda && m->stuck_clocks <= 9) {
			m->stuck_sda = 0;
			m->sda_not_recovered = 0;
		} else if (m->stuck_sda) {
			m->sda_not_recovered = 1;
		}
		m->recovery_pending = 0;
		m->enable &= ~8u;
		spin_unlock_irqrestore(&m->lock, fl);
		return 0;
	}
	if (m->stuck_sda || m->abrt_hold) {
		spin_unlock_irqrestore(&m->lock, fl);
		return 0;
	}
	if (!m->tx_count) {
		if (m->in_txn) {
			if (m->hold_on_empty) {
				m->hold = 1;
			} else {
				end_txn(m, 1);
			}
		}
		spin_unlock_irqrestore(&m->lock, fl);
		return 0;
	}
	m->hold = 0;
	/* A stretching target holds SCL before every further byte. */
	if (m->in_txn && m->cur && m->cur->stretch_us && !m->stretched) {
		m->stretched = 1;
		spin_unlock_irqrestore(&m->lock, fl);
		return m->cur->stretch_us;
	}
	m->stretched = 0;
	cmd = m->tx[m->tx_head];
	m->tx_head = (m->tx_head + 1) % 256;
	m->tx_count--;
	read = (cmd & CMD_READ) != 0;

	if (!m->in_txn) {
		if (!address_phase(m, read, 0))
			goto out;
	} else if ((cmd & CMD_RESTART) || read != m->cur_read) {
		if (!(m->con & CON_RESTART_EN)) {
			end_txn(m, 0);
			if (!address_phase(m, read, 0))
				goto out;
		} else if (!address_phase(m, read, 1)) {
			goto out;
		}
	}

	if (!read) {
		uint8_t b = (uint8_t)cmd;

		logev(m, EV_WRITE, b);
		if (!slv_write(m->cur, b)) {
			logev(m, EV_NACK_DATA, 0);
			do_abort(m, ABRT_TXDATA_NOACK, 1);
			goto out;
		}
	} else {
		uint8_t b = slv_read(m->cur);

		logev(m, EV_READ, b);
		if (m->rx_count >= m->rx_depth) {
			m->raw_latched |= I_RX_OVER;
			m->rx_over++;
		} else {
			m->rx[(m->rx_head + m->rx_count) % 256] = b;
			m->rx_count++;
		}
	}
	if (cmd & CMD_STOP)
		end_txn(m, 1);
out:
	spin_unlock_irqrestore(&m->lock, fl);
	return 1;
}

static void dwm_thread(void *arg)
{
	struct dw_model *m = arg;

	while (m->running) {
		unsigned int r = dwm_step(m);

		if (r > 1)
			h_usleep(r);
		else if (!r)
			h_usleep(m->tick_us ? m->tick_us : 10);
		else if (m->tick_us)
			h_usleep(m->tick_us);
		else
			h_yield();
	}
	__atomic_store_n(&m->exited, 1, __ATOMIC_RELEASE);
}

struct dw_model *dwm_create(unsigned int tx_depth, unsigned int rx_depth)
{
	struct dw_model *m = kcalloc(1, sizeof(*m));
	int slot = -1;

	T_ASSERT(m);
	spinlock_init(&m->lock, "dw_model");
	m->comp_type = 0x44570140u;
	m->version = 0x3132312Au; /* 1.21a */
	m->tx_depth = tx_depth;
	m->rx_depth = rx_depth;
	m->param1 = ((tx_depth - 1) << 16) | ((rx_depth - 1) << 8) |
		    (3u << 2) /* high speed */ | 2u /* 32-bit APB */;
	m->hold_on_empty = 1;
	m->disable_latency = 2;
	m->fs_spklen = 7;
	m->hs_spklen = 2;
	m->con = 0x7f; /* reset value: master, fast, restart, slave off */
	m->sda_hold = 1;
	m->tick_us = 2;
	m->scl_gpio = 1;
	m->sda_gpio_drive = 1;
	for (int i = 0; i < MAX_MODELS; i++)
		if (!g_models[i]) {
			slot = i;
			break;
		}
	T_ASSERT(slot >= 0);
	g_models[slot] = m;
	m->running = 1;
	T_ASSERT(sched_add_task(dwm_thread, m, NULL, 0));
	return m;
}

void dwm_destroy(struct dw_model *m)
{
	m->running = 0;
	while (!__atomic_load_n(&m->exited, __ATOMIC_ACQUIRE))
		h_usleep(50);
	for (int i = 0; i < MAX_MODELS; i++)
		if (g_models[i] == m)
			g_models[i] = NULL;
	for (int i = 0; i < m->nslaves; i++)
		kfree(m->slaves[i]);
	kfree(m);
}

void dwm_add_slave(struct dw_model *m, struct dwm_slave *s)
{
	uint64_t fl;

	spin_lock_irqsave(&m->lock, &fl);
	T_ASSERT(m->nslaves < DWM_MAX_SLAVES);
	m->slaves[m->nslaves++] = s;
	spin_unlock_irqrestore(&m->lock, fl);
}

void dwm_log_clear(struct dw_model *m)
{
	uint64_t fl;

	spin_lock_irqsave(&m->lock, &fl);
	m->nlog = 0;
	spin_unlock_irqrestore(&m->lock, fl);
}

void dwm_log_text(struct dw_model *m, char *buf, int cap)
{
	int p = 0;
	uint64_t fl;

	buf[0] = 0;
	spin_lock_irqsave(&m->lock, &fl);
	for (int i = 0; i < m->nlog && p < cap - 16; i++) {
		struct dwm_event *e = &m->log[i];

		switch (e->type) {
		case EV_START:
			p += ksnprintf(buf + p, cap - p, "S ");
			break;
		case EV_RESTART:
			p += ksnprintf(buf + p, cap - p, "Sr ");
			break;
		case EV_ADDR:
			p += ksnprintf(buf + p, cap - p, "%x%s%s ",
				       e->val & 0x3ff,
				       (e->val & 0x4000) ? "t" : "",
				       (e->val & 0x8000) ? "R" : "W");
			break;
		case EV_NACK_ADDR:
			p += ksnprintf(buf + p, cap - p, "NA ");
			break;
		case EV_WRITE:
			p += ksnprintf(buf + p, cap - p, "w%02x ", e->val);
			break;
		case EV_READ:
			p += ksnprintf(buf + p, cap - p, "r%02x ", e->val);
			break;
		case EV_NACK_DATA:
			p += ksnprintf(buf + p, cap - p, "ND ");
			break;
		case EV_STOP:
			p += ksnprintf(buf + p, cap - p, "P ");
			break;
		case EV_ABORT:
			p += ksnprintf(buf + p, cap - p, "A%d ", e->val);
			break;
		case EV_ARB_LOST:
			p += ksnprintf(buf + p, cap - p, "AL ");
			break;
		case EV_RECOVER:
			p += ksnprintf(buf + p, cap - p, "REC ");
			break;
		}
	}
	spin_unlock_irqrestore(&m->lock, fl);
	if (p > 0 && buf[p - 1] == ' ')
		buf[p - 1] = 0;
}

/* ==========================================================================
 * Recovery lines: SCL/SDA driven by GPIOs during a generic recovery
 * ========================================================================== */

int dwm_get_scl(struct dw_model *m)
{
	return m->scl_gpio;
}

int dwm_get_sda(struct dw_model *m)
{
	return !m->stuck_sda && m->sda_gpio_drive;
}

void dwm_set_scl(struct dw_model *m, int v)
{
	uint64_t fl;

	spin_lock_irqsave(&m->lock, &fl);
	/* a rising edge clocks the stuck target one bit further */
	if (v && !m->scl_gpio && m->stuck_sda && --m->stuck_clocks <= 0)
		m->stuck_sda = 0;
	m->scl_gpio = v;
	spin_unlock_irqrestore(&m->lock, fl);
}

void dwm_set_sda(struct dw_model *m, int v)
{
	m->sda_gpio_drive = v;
}
