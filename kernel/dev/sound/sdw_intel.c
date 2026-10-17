// LikeOS Intel SoundWire master: the Intel glue around the Cadence
// SoundWire IP, for cAVS 1.8/2.0/2.5 (Cannon Lake .. Raptor Lake) and ACE
// 1.x (Meteor/Arrow Lake), 2.x (Lunar Lake) and 3.x (Panther Lake).
//
// Register blocks:
//
//   cAVS, ACE1  (DSP BAR):  SHIM at 0x2c000 (ACE1: 0x38000), one for all
//               links: link capabilities, link power (LCTL SPA/CPA), the
//               sync generator, per-link I/O control and the PCM stream
//               (PDI) to DSP gateway map.  ALH at 0x2c800 (ACE1: 0x24000):
//               the audio link hub stream configuration.  The Cadence
//               block of link n at 0x30000 + 0x10000 * n.
//   ACE2, ACE3  (HDA BAR):  the links are "sublinks" of an HDA multi-link
//               extended link: power through the ML LCTL, the sync
//               generator in ML LSYNC.  Per link a generic SHIM at
//               0x30000 + 0x8000 * n, the Cadence block 0x100 above it
//               (its IP-specific half another 0x4000 up) and a vendor
//               SHIM at +0x6000.
//
// The Cadence master IP: MCP_CONFIG/CONTROL, frame shape and clock
// divider, the command FIFO (up to 8 commands, responses read back from
// the same window), slave status (2 bits per device), interrupts, the
// bank 0/1 data port (DPN) registers and the PDI configuration.
//
// Commands are polled, never waited for by interrupt: the response
// watermark interrupt stays masked, so a transfer is atomic-context safe
// and does not depend on the DSP's interrupt routing.  Peripheral status
// changes do raise the interrupt (when it is routed), and are serviced
// from the sound work thread; a periodic poll covers the rest.
//
// Copyright (C) 2026 The LikeOS Project

#include "sdw.h"
#include <kernel/mm/memory.h>
#include <kernel/ke/syscall.h>

/* ==========================================================================
 * Register definitions
 * ========================================================================== */

/* cAVS / ACE1 shared SHIM */
#define SHIM_BASE_CAVS 0x2c000u
#define ALH_BASE_CAVS 0x2c800u
#define SHIM_BASE_ACE1 0x38000u
#define ALH_BASE_ACE1 0x24000u
#define LINK_BASE 0x30000u
#define LINK_SIZE 0x10000u

#define SHIM_LCAP 0x00
#define SHIM_LCAP_LCOUNT 0x7u
#define SHIM_LCAP_MLCS (1u << 8)
#define SHIM_LCTL 0x04
#define SHIM_LCTL_SPA(l) (1u << (l))
#define SHIM_LCTL_CPA(l) (1u << (8 + (l)))
#define SHIM_LCTL_MLCS_MASK (0x7u << 27)
#define SHIM_SYNC 0x0c
#define SHIM_SYNC_SYNCPRD_MASK 0x7fffu
#define SHIM_SYNC_SYNCCPU (1u << 15)
#define SHIM_SYNC_CMDSYNC(l) (1u << (16 + (l)))
#define SHIM_SYNC_CMDSYNC_MASK (0xfu << 16)
#define SHIM_SYNC_SYNCGO (1u << 24)
#define SHIM_CTLSCAP(l) (0x010u + 0x60u * (l))
#define SHIM_PCMSCAP(l) (0x020u + 0x60u * (l))
#define SHIM_PCMSYCHM(l, y) (0x022u + 0x60u * (l) + 2u * (y))
#define SHIM_PCMSYCHC(l, y) (0x042u + 0x60u * (l) + 2u * (y))
#define SHIM_IOCTL(l) (0x06cu + 0x60u * (l))
#define SHIM_CTMCTL(l) (0x06eu + 0x60u * (l))
#define SHIM_WAKEEN 0x190
#define SHIM_WAKESTS 0x192

#define PCMSCAP_ISS 0x000fu
#define PCMSCAP_OSS 0x00f0u
#define PCMSCAP_BSS 0x1f00u
#define PCMSYCM_LCHN 0x000fu
#define PCMSYCM_HCHN 0x00f0u
#define PCMSYCM_STREAM 0x3f00u
#define PCMSYCM_DIR 0x8000u

#define IOCTL_MIF (1u << 0)
#define IOCTL_CO (1u << 1)
#define IOCTL_COE (1u << 2)
#define IOCTL_DO (1u << 3)
#define IOCTL_DOE (1u << 4)
#define IOCTL_BKE (1u << 5)
#define IOCTL_WPDD (1u << 6)
#define CTMCTL_DACTQE (1u << 0)
#define CTMCTL_DODS (1u << 1)
#define CTMCTL_DOAIS (0x3u << 3)

#define ALH_STRMZCFG(x) (4u * (x))
#define ALH_NUM_STREAMS 64
#define ALH_DMAT_MASK 0xffu
#define ALH_DMAT_VAL 0x3u
#define ALH_CHN_MASK (0xfu << 16)

/* clock sources (LCTL.MLCS / LVSCTL.MLCS) */
#define MLCS_XTAL 0x0
#define MLCS_CARDINAL 0x1
#define MLCS_AUDIO_PLL 0x2

/* the sync generator runs at 4 kHz */
#define GSYNC_KHZ 4u
#define SYNCPRD_24 (24000u / GSYNC_KHZ - 1)
#define SYNCPRD_24_576 (24576u / GSYNC_KHZ - 1)
#define SYNCPRD_38_4 (38400u / GSYNC_KHZ - 1)
#define SYNCPRD_96 (96000u / GSYNC_KHZ - 1)

/* ACE2+: per-link SHIM2 in the HDA BAR */
#define SHIM2_GENERIC(x) (0x00030000u + 0x8000u * (x))
#define SHIM2_IP(x) (0x00030100u + 0x8000u * (x))
#define SHIM2_VS(x) (0x00036000u + 0x8000u * (x))
#define SHIM2_INSTANCE 0x8000u
#define SHIM2_IP_OFF 0x100u
#define SHIM2_VS_OFF 0x6000u
#define SHIM2_LECAP 0x00
#define SHIM2_PCMSCAP 0x10
#define SHIM2_PCMSYCHC(y) (0x14u + 4u * (y))
#define SHIM2_PCMSYCHM(y) (0x16u + 4u * (y))
#define VS_LVSCTL 0x04
#define VS_LVSCTL_MLCS (0x7u << 27)
#define VS_ACTMCTL 0x0e
#define VS_ACT_DACTQE (1u << 0)
#define VS_ACT_DODS (1u << 1)
#define VS_ACT_DODSE (1u << 2)
#define VS_ACT_DOAIS (0x3u << 3)
#define VS_ACT_DOAISE (1u << 5)
#define VS_ACT_CLSS (1u << 6)
#define VS_ACT_CLDS (0x1fu << 7)
#define VS_ACT_DODSE2 (0x3u << 12)
#define VS_ACT_DOAISE2 (1u << 14)
#define VS_ACT_CLDE (1u << 15)
#define CADENCE_IP_OFFSET 0x4000u

/* HDA multi-link capability (BAR0) */
#define HDA_LLCH 0x14
#define HDA_CAP_ML 0x2
#define ML_MLCD 0x04
#define ML_LINK(i) (0x40u + 0x40u * (i))
#define ML_LCAP 0x00
#define ML_LCAP_ALT (1u << 28)
#define ML_LCAP_INTC (1u << 27)
#define ML_LCAP_OFLS (1u << 26)
#define ML_LCAP_LSS (1u << 23)
#define ML_LCAP_SLCOUNT (0x7u << 20)
#define ML_LCTL 0x04
#define ML_LCTL_INTSTS (1u << 31)
#define ML_LCTL_CPA(s) (1u << (23 + (s)))
#define ML_LCTL_SPA(s) (1u << (16 + (s)))
#define ML_LCTL_INTEN (1u << 5)
#define ML_LSDIID(s) (0x0cu + 2u * (s))
#define ML_LSYNC 0x1c
#define ML_LSYNC_CMDSYNC(s) (1u << (24 + (s)))
#define ML_LSYNC_SYNCGO (1u << 23)
#define ML_LSYNC_SYNCPU (1u << 20)
#define ML_LSYNC_SYNCPRD 0xfffffu
#define ML_LEPTR 0x20
#define ML_LEPTR_ID(v) (((v) >> 24) & 0xffu)
#define ML_LEPTR_PTR(v) ((v) & 0xfffffu)
#define ML_LEPTR_ID_SDW 0x00

/* DSP-level SoundWire interrupt routing (DSP BAR) */
#define CAVS_ADSPIC2 0x10
#define CAVS_ADSPIS2 0x14
#define CAVS_ADSPIx2_SNDW (1u << 5)
#define MTL_HFINTIPPTR 0x1108
#define MTL_HFINTIPPTR_MASK 0x1fffffu
#define MTL_IRQ_SNDW (1u << 6)
#define MTL_HFSNDWIE 0x114c
#define MTL_HFSNDWIE_MASK 0xfu
#define MTL_IRQSTS 0x20

/* Cadence master IP */
#define CDNS_MCP_CONFIG 0x00
#define CDNS_MCP_CONFIG_BUS_REL (1u << 6)
#define CDNS_IP_MCP_CONFIG 0x00
#define CDNS_IP_MCP_CONFIG_MCMD_RETRY (0xfu << 24)
#define CDNS_IP_MCP_CONFIG_MPREQ_DELAY (0x1fu << 16)
#define CDNS_IP_MCP_CONFIG_MMASTER (1u << 7)
#define CDNS_IP_MCP_CONFIG_SNIFFER (1u << 5)
#define CDNS_IP_MCP_CONFIG_CMD (1u << 3)
#define CDNS_IP_MCP_CONFIG_OP 0x7u
#define CDNS_MCP_CONTROL 0x04
#define CDNS_MCP_CONTROL_CMD_RST (1u << 7)
#define CDNS_MCP_CONTROL_SOFT_RST (1u << 6)
#define CDNS_MCP_CONTROL_HW_RST (1u << 4)
#define CDNS_MCP_CONTROL_CLK_STOP_CLR (1u << 2)
#define CDNS_IP_MCP_CONTROL 0x04
#define CDNS_IP_MCP_CONTROL_SW_RST (1u << 5)
#define CDNS_IP_MCP_CONTROL_CLK_PAUSE (1u << 3)
#define CDNS_IP_MCP_CONTROL_CMD_ACCEPT (1u << 1)
#define CDNS_IP_MCP_CONTROL_BLOCK_WAKEUP (1u << 0)
#define CDNS_IP_MCP_CMDCTRL 0x08
#define CDNS_MCP_SSPSTAT 0x0c
#define CDNS_MCP_FRAME_SHAPE 0x10
#define CDNS_MCP_FRAME_SHAPE_INIT 0x14
#define CDNS_MCP_CONFIG_UPDATE 0x18
#define CDNS_MCP_CONFIG_UPDATE_BIT 1u
#define CDNS_MCP_PHYCTRL 0x1c
#define CDNS_MCP_SSP_CTRL0 0x20
#define CDNS_MCP_SSP_CTRL1 0x28
#define CDNS_MCP_CLK_CTRL0 0x30
#define CDNS_MCP_CLK_CTRL1 0x38
#define CDNS_MCP_CLK_MCLKD_MASK 0xffu
#define CDNS_MCP_STAT 0x40
#define CDNS_MCP_STAT_ACTIVE_BANK (1u << 20)
#define CDNS_MCP_STAT_CLK_STOP (1u << 16)
#define CDNS_MCP_INTSTAT 0x44
#define CDNS_MCP_INTMASK 0x48
#define CDNS_MCP_INT_IRQ (1u << 31)
#define CDNS_MCP_INT_RESERVED1 (0x3fffu << 17)
#define CDNS_MCP_INT_WAKEUP (1u << 16)
#define CDNS_MCP_INT_SLAVE_RSVD (1u << 15)
#define CDNS_MCP_INT_SLAVE_ALERT (1u << 14)
#define CDNS_MCP_INT_SLAVE_ATTACH (1u << 13)
#define CDNS_MCP_INT_SLAVE_NATTACH (1u << 12)
#define CDNS_MCP_INT_SLAVE_MASK (0xfu << 12)
#define CDNS_MCP_INT_DPINT (1u << 11)
#define CDNS_MCP_INT_CTRL_CLASH (1u << 10)
#define CDNS_MCP_INT_DATA_CLASH (1u << 9)
#define CDNS_MCP_INT_PARITY (1u << 8)
#define CDNS_MCP_INT_CMD_ERR (1u << 7)
#define CDNS_MCP_INT_RESERVED2 (0x7u << 4)
#define CDNS_MCP_INT_RX_NE (1u << 3)
#define CDNS_MCP_INT_RX_WL (1u << 2)
#define CDNS_MCP_INT_TXE (1u << 1)
#define CDNS_MCP_INT_TXF (1u << 0)
#define CDNS_MCP_INT_RESERVED (CDNS_MCP_INT_RESERVED1 | CDNS_MCP_INT_RESERVED2)
#define CDNS_MCP_INTSET 0x4c
#define CDNS_MCP_SLAVE_STAT 0x50
#define CDNS_MCP_SLAVE_INTSTAT0 0x54
#define CDNS_MCP_SLAVE_INTSTAT1 0x58
#define CDNS_MCP_SLAVE_INTMASK0 0x5c
#define CDNS_MCP_SLAVE_INTMASK1 0x60
#define CDNS_MCP_PORT_INTSTAT 0x64
#define CDNS_MCP_PDI_STAT 0x6c
#define CDNS_MCP_FIFOLEVEL 0x78
#define CDNS_MCP_FIFOSTAT 0x7c
#define CDNS_MCP_RX_FIFO_AVAIL 0x3fu
#define CDNS_IP_MCP_CMD_BASE 0x80
#define CDNS_MCP_CMD_LEN 8
#define CDNS_MCP_CMD_SSP_TAG (1u << 31)
#define CDNS_MCP_CMD_COMMAND_SHIFT 28
#define CDNS_MCP_CMD_DEV_ADDR_SHIFT 24
#define CDNS_MCP_CMD_REG_ADDR_SHIFT 8
#define CDNS_MCP_CMD_READ 2u
#define CDNS_MCP_CMD_WRITE 3u
#define CDNS_MCP_RESP_ACK (1u << 0)
#define CDNS_MCP_RESP_NACK (1u << 1)
#define CDNS_MCP_RESP_RDATA(r) (((r) >> 8) & 0xffu)

#define CDNS_DP_SIZE 0x80u
#define CDNS_DPN_B0_CONFIG(n) (0x100u + CDNS_DP_SIZE * (n))
#define CDNS_DPN_B0_CH_EN(n) (0x104u + CDNS_DP_SIZE * (n))
#define CDNS_DPN_B0_SAMPLE_CTRL(n) (0x108u + CDNS_DP_SIZE * (n))
#define CDNS_DPN_B0_OFFSET_CTRL(n) (0x10cu + CDNS_DP_SIZE * (n))
#define CDNS_DPN_B0_HCTRL(n) (0x110u + CDNS_DP_SIZE * (n))
#define CDNS_DPN_B0_ASYNC_CTRL(n) (0x114u + CDNS_DP_SIZE * (n))
#define CDNS_DPN_B1_OFFSET 0x18u /* bank 1 = bank 0 + this */
#define CDNS_DPN_CONFIG_BPM (1u << 18)
#define CDNS_DPN_CONFIG_BGC (0x3u << 16)
#define CDNS_DPN_CONFIG_WL (0x1fu << 8)
#define CDNS_DPN_CONFIG_PORT_DAT (0x3u << 2)
#define CDNS_DPN_CONFIG_PORT_FLOW 0x3u
#define CDNS_DPN_HCTRL_HSTOP 0xfu
#define CDNS_DPN_HCTRL_HSTART (0xfu << 4)
#define CDNS_DPN_HCTRL_LCTRL (0x7u << 8)
#define CDNS_PORTCTRL(n) (0x130u + CDNS_DP_SIZE * (n))
#define CDNS_PORTCTRL_TEST_FAILED (1u << 1)
#define CDNS_PORTCTRL_DIRN (1u << 7)
#define CDNS_PORTCTRL_BANK_INVERT (1u << 8)
#define CDNS_PORTCTRL_BULK_ENABLE (1u << 16)
#define CDNS_PDI_CONFIG(n) (0x1100u + 16u * (n))
#define CDNS_PDI_CONFIG_SOFT_RESET (1u << 24)
#define CDNS_PDI_CONFIG_CHANNEL (0xffu << 8)
#define CDNS_PDI_CONFIG_PORT 0x1fu

#define CDNS_DOUBLE_RATE 2u
#define CDNS_RESET_ITERATIONS 10
#define CDNS_XFER_TIMEOUT_US 4000u
#define CDNS_DEAD_AFTER_TIMEOUTS 16

/* ==========================================================================
 * Accessors
 * ========================================================================== */

static inline uint32_t mmio_rd32(volatile uint8_t *base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static inline void mmio_wr32(volatile uint8_t *base, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(base + off) = v;
}

static inline uint16_t mmio_rd16(volatile uint8_t *base, uint32_t off)
{
	return *(volatile uint16_t *)(base + off);
}

static inline void mmio_wr16(volatile uint8_t *base, uint32_t off, uint16_t v)
{
	*(volatile uint16_t *)(base + off) = v;
}

static inline uint32_t cdns_rd(struct sdw_link *l, uint32_t off)
{
	return mmio_rd32(l->regs, off);
}

static inline void cdns_wr(struct sdw_link *l, uint32_t off, uint32_t v)
{
	mmio_wr32(l->regs, off, v);
}

static inline void cdns_upd(struct sdw_link *l, uint32_t off, uint32_t mask,
			    uint32_t val)
{
	uint32_t t = cdns_rd(l, off);
	cdns_wr(l, off, (t & ~mask) | (val & mask));
}

static inline uint32_t cdns_ip_rd(struct sdw_link *l, uint32_t off)
{
	return mmio_rd32(l->regs, l->ip_offset + off);
}

static inline void cdns_ip_wr(struct sdw_link *l, uint32_t off, uint32_t v)
{
	mmio_wr32(l->regs, l->ip_offset + off, v);
}

static inline void cdns_ip_upd(struct sdw_link *l, uint32_t off,
			       uint32_t mask, uint32_t val)
{
	uint32_t t = cdns_ip_rd(l, off);
	cdns_ip_wr(l, off, (t & ~mask) | (val & mask));
}

/* Poll until (reg & mask) == want; bounded.  0 or -ETIMEDOUT. */
static int cdns_wait(struct sdw_link *l, uint32_t off, uint32_t mask,
		     uint32_t want, uint32_t tries)
{
	for (uint32_t i = 0; i < tries; i++) {
		uint32_t v = cdns_rd(l, off);
		if (v == 0xffffffffu)
			return -ENODEV;
		if ((v & mask) == want)
			return 0;
		snd_udelay(50);
	}
	return -ETIMEDOUT;
}

static int cdns_config_update(struct sdw_link *l)
{
	if (cdns_rd(l, CDNS_MCP_STAT) & CDNS_MCP_STAT_CLK_STOP)
		return -EINVAL;
	cdns_wr(l, CDNS_MCP_CONFIG_UPDATE, CDNS_MCP_CONFIG_UPDATE_BIT);
	return cdns_wait(l, CDNS_MCP_CONFIG_UPDATE, CDNS_MCP_CONFIG_UPDATE_BIT,
			 0, 40);
}

/* ==========================================================================
 * Frame shapes
 * ========================================================================== */

const uint16_t sdw_rows[24] = { 48,  50,  60,  64,  75,  80,  125, 147,
				96,  100, 120, 128, 150, 160, 250, 0,
				192, 200, 240, 256, 72,  144, 90,  180 };
const uint8_t sdw_cols[8] = { 2, 4, 6, 8, 10, 12, 14, 16 };

int sdw_find_row_index(uint32_t row)
{
	for (int i = 0; i < 24; i++)
		if (sdw_rows[i] && sdw_rows[i] == row)
			return i;
	return -1;
}

int sdw_find_col_index(uint32_t col)
{
	for (int i = 0; i < 8; i++)
		if (sdw_cols[i] == col)
			return i;
	return -1;
}

/* ==========================================================================
 * Command FIFO
 * ========================================================================== */

/* Drop responses left over from an aborted transaction. */
static void cdns_drain_rx(struct sdw_link *l)
{
	uint32_t avail = cdns_rd(l, CDNS_MCP_FIFOSTAT) & CDNS_MCP_RX_FIFO_AVAIL;

	for (uint32_t i = 0; i < avail && i < 64; i++)
		(void)cdns_ip_rd(l, CDNS_IP_MCP_CMD_BASE +
					    4u * (i % CDNS_MCP_CMD_LEN));
	cdns_wr(l, CDNS_MCP_INTSTAT, CDNS_MCP_INT_RX_WL);
}

/* Send `n' command words, collect `n' responses. */
static int cdns_batch(struct sdw_link *l, const uint32_t *cmd, int n,
		      uint32_t *resp)
{
	uint32_t waited = 0;

	if (n < 1 || n > CDNS_MCP_CMD_LEN)
		return SDW_CMD_FAIL;
	if (l->msg_count != (uint32_t)n) {
		cdns_wr(l, CDNS_MCP_FIFOLEVEL, (uint32_t)n);
		l->msg_count = (uint32_t)n;
	}
	if (cdns_rd(l, CDNS_MCP_FIFOSTAT) & CDNS_MCP_RX_FIFO_AVAIL)
		cdns_drain_rx(l);
	else
		cdns_wr(l, CDNS_MCP_INTSTAT, CDNS_MCP_INT_RX_WL);

	for (int i = 0; i < n; i++)
		cdns_ip_wr(l, CDNS_IP_MCP_CMD_BASE + 4u * (uint32_t)i, cmd[i]);

	for (;;) {
		uint32_t st = cdns_rd(l, CDNS_MCP_INTSTAT);
		uint32_t avail;

		if (st == 0xffffffffu) {
			l->consecutive_timeouts = CDNS_DEAD_AFTER_TIMEOUTS;
			return SDW_CMD_TIMEOUT;
		}
		avail = cdns_rd(l, CDNS_MCP_FIFOSTAT) & CDNS_MCP_RX_FIFO_AVAIL;
		if (avail >= (uint32_t)n || (st & CDNS_MCP_INT_RX_WL))
			break;
		if (waited >= CDNS_XFER_TIMEOUT_US) {
			cdns_drain_rx(l);
			return SDW_CMD_TIMEOUT;
		}
		snd_udelay(5);
		waited += 5;
	}
	for (int i = 0; i < n; i++)
		resp[i] = cdns_ip_rd(l, CDNS_IP_MCP_CMD_BASE + 4u * (uint32_t)i);
	cdns_wr(l, CDNS_MCP_INTSTAT, CDNS_MCP_INT_RX_WL);
	return SDW_CMD_OK;
}

static uint32_t cdns_cmd_word(int dev, uint32_t cmd, uint16_t addr,
			      uint8_t data, int ssp)
{
	uint32_t w = ((uint32_t)(dev & 0xf) << CDNS_MCP_CMD_DEV_ADDR_SHIFT) |
		     (cmd << CDNS_MCP_CMD_COMMAND_SHIFT) |
		     ((uint32_t)addr << CDNS_MCP_CMD_REG_ADDR_SHIFT) | data;
	if (ssp)
		w |= CDNS_MCP_CMD_SSP_TAG;
	return w;
}

static int cdns_eval_resp(const uint32_t *resp, int n)
{
	int nack = 0, no_ack = 0;

	for (int i = 0; i < n; i++) {
		if (resp[i] & CDNS_MCP_RESP_NACK)
			nack = 1;
		else if (!(resp[i] & CDNS_MCP_RESP_ACK))
			no_ack = 1;
	}
	if (nack)
		return SDW_CMD_FAIL;
	if (no_ack)
		return SDW_CMD_IGNORED;
	return SDW_CMD_OK;
}

static void cdns_note_result(struct sdw_link *l, int rc)
{
	l->st_cmds++;
	switch (rc) {
	case SDW_CMD_OK:
		l->consecutive_timeouts = 0;
		break;
	case SDW_CMD_IGNORED:
		l->st_ignored++;
		l->consecutive_timeouts = 0;
		break;
	case SDW_CMD_FAIL:
		l->st_nak++;
		l->consecutive_timeouts = 0;
		break;
	default:
		l->st_timeout++;
		if (++l->consecutive_timeouts >= CDNS_DEAD_AFTER_TIMEOUTS &&
		    !l->dead) {
			l->dead = 1;
			sdw_info("link %d stopped answering commands; disabled\n",
				 l->id);
		}
		break;
	}
}

int sdw_cdns_xfer(struct sdw_link *l, int dev_num, uint16_t addr, int read,
		  uint8_t *buf, int count, int ssp_sync, int page,
		  uint8_t page1, uint8_t page2)
{
	uint32_t cmd[CDNS_MCP_CMD_LEN], resp[CDNS_MCP_CMD_LEN];
	int rc;

	if (!l || !l->up || l->dead)
		return SDW_CMD_TIMEOUT;
	if (count < 1 || count > CDNS_MCP_CMD_LEN || !buf || dev_num < 0 ||
	    dev_num > 15)
		return SDW_CMD_FAIL;

	if (page) {
		cmd[0] = cdns_cmd_word(dev_num, CDNS_MCP_CMD_WRITE,
				       SDW_SCP_ADDRPAGE1, page1, 0);
		cmd[1] = cdns_cmd_word(dev_num, CDNS_MCP_CMD_WRITE,
				       SDW_SCP_ADDRPAGE2, page2, 0);
		rc = cdns_batch(l, cmd, 2, resp);
		if (rc == SDW_CMD_OK)
			rc = cdns_eval_resp(resp, 2);
		if (rc != SDW_CMD_OK) {
			cdns_note_result(l, rc);
			return rc;
		}
	}

	for (int i = 0; i < count; i++)
		cmd[i] = cdns_cmd_word(dev_num,
				       read ? CDNS_MCP_CMD_READ :
					      CDNS_MCP_CMD_WRITE,
				       (uint16_t)(addr + i), read ? 0 : buf[i],
				       ssp_sync);
	rc = cdns_batch(l, cmd, count, resp);
	if (rc == SDW_CMD_OK) {
		rc = cdns_eval_resp(resp, count);
		if (rc == SDW_CMD_OK && read) {
			for (int i = 0; i < count; i++)
				buf[i] = (uint8_t)CDNS_MCP_RESP_RDATA(resp[i]);
		}
	}
	cdns_note_result(l, rc);
	return rc;
}

uint32_t sdw_cdns_slave_stat(struct sdw_link *l)
{
	if (!l || !l->up || l->dead)
		return 0;
	return cdns_rd(l, CDNS_MCP_SLAVE_STAT);
}

void sdw_cdns_ack_slave_intstat(struct sdw_link *l, uint32_t *s0,
				uint32_t *s1)
{
	uint32_t a = 0, b = 0;

	if (l && l->up && !l->dead) {
		cdns_wr(l, CDNS_MCP_INTSTAT, CDNS_MCP_INT_SLAVE_MASK);
		a = cdns_rd(l, CDNS_MCP_SLAVE_INTSTAT0);
		b = cdns_rd(l, CDNS_MCP_SLAVE_INTSTAT1);
		if (a == 0xffffffffu)
			a = b = 0;
		cdns_wr(l, CDNS_MCP_SLAVE_INTSTAT0, a);
		cdns_wr(l, CDNS_MCP_SLAVE_INTSTAT1, b);
	}
	if (s0)
		*s0 = a;
	if (s1)
		*s1 = b;
}

void sdw_cdns_slave_irq_enable(struct sdw_link *l, int on)
{
	uint64_t fl;

	if (!l || !l->up)
		return;
	spin_lock_irqsave(&l->irq_lock, &fl);
	cdns_upd(l, CDNS_MCP_INTMASK, CDNS_MCP_INT_SLAVE_MASK,
		 on ? CDNS_MCP_INT_SLAVE_MASK : 0);
	spin_unlock_irqrestore(&l->irq_lock, fl);
}

/* ==========================================================================
 * Master data ports
 * ========================================================================== */

void sdw_cdns_dpn_params(struct sdw_link *l, int bank,
			 const struct sdw_port_rt *p)
{
	uint32_t boff = bank ? CDNS_DPN_B1_OFFSET : 0;
	uint32_t cfg, v;
	int n;

	if (!l || !l->up || l->dead || !p)
		return;
	n = p->num;
	if (n < 0 || n >= SDW_MAX_PDI)
		return;
	cfg = cdns_rd(l, CDNS_DPN_B0_CONFIG(n) + boff);
	cfg = sdw_set_bits(cfg, CDNS_DPN_CONFIG_WL, p->bps ? p->bps - 1 : 0);
	cfg = sdw_set_bits(cfg, CDNS_DPN_CONFIG_PORT_FLOW, 0); /* isochronous */
	cfg = sdw_set_bits(cfg, CDNS_DPN_CONFIG_PORT_DAT, 0); /* normal data */
	cfg = sdw_set_bits(cfg, CDNS_DPN_CONFIG_BGC, p->blk_grp_ctrl);
	cfg = sdw_set_bits(cfg, CDNS_DPN_CONFIG_BPM, p->blk_pkg_mode);
	cdns_wr(l, CDNS_DPN_B0_CONFIG(n) + boff, cfg);

	v = (p->offset1 & 0xff) | ((p->offset2 & 0xff) << 8);
	cdns_wr(l, CDNS_DPN_B0_OFFSET_CTRL(n) + boff, v);
	v = sdw_set_bits(0, CDNS_DPN_HCTRL_HSTART, p->hstart);
	v = sdw_set_bits(v, CDNS_DPN_HCTRL_HSTOP, p->hstop);
	v = sdw_set_bits(v, CDNS_DPN_HCTRL_LCTRL, p->lane);
	cdns_wr(l, CDNS_DPN_B0_HCTRL(n) + boff, v);
	cdns_wr(l, CDNS_DPN_B0_SAMPLE_CTRL(n) + boff,
		p->sample_interval ? (p->sample_interval - 1) & 0xffff : 0);
}

void sdw_cdns_dpn_enable(struct sdw_link *l, int bank, int port,
			 uint32_t ch_mask)
{
	uint32_t boff = bank ? CDNS_DPN_B1_OFFSET : 0;

	if (!l || !l->up || l->dead || port < 0 || port >= SDW_MAX_PDI)
		return;
	cdns_wr(l, CDNS_DPN_B0_CH_EN(port) + boff, ch_mask & 0xff);
}

void sdw_cdns_bus_conf(struct sdw_link *l, int bank, uint32_t curr_dr_freq)
{
	uint32_t div;

	if (!l || !l->up || l->dead || !curr_dr_freq)
		return;
	div = l->mprops.mclk_freq * CDNS_DOUBLE_RATE / curr_dr_freq;
	if (div)
		div--;
	if (div > 0xff)
		div = 0xff;
	cdns_upd(l, bank ? CDNS_MCP_CLK_CTRL1 : CDNS_MCP_CLK_CTRL0,
		 CDNS_MCP_CLK_MCLKD_MASK, div);
}

int sdw_cdns_active_bank(struct sdw_link *l)
{
	uint32_t st;

	if (!l || !l->up || l->dead)
		return -ENODEV;
	st = cdns_rd(l, CDNS_MCP_STAT);
	if (st == 0xffffffffu)
		return -ENODEV;
	return (st & CDNS_MCP_STAT_ACTIVE_BANK) ? 1 : 0;
}

/* ==========================================================================
 * PDIs and the DSP gateways
 * ========================================================================== */

static int pdi_alh_id(int link, int pdi)
{
	/* the bulk and PCM streams are not contiguous in the ALH */
	int id = link * 16 + pdi + 3;
	if (pdi >= 2)
		id += 2;
	return id;
}

int sdw_intel_alh_to_pdi(int link, int alh_id)
{
	int rel = alh_id - link * 16 - 3;

	if (rel < 0)
		return -EINVAL;
	if (rel >= 4)
		rel -= 2;
	else if (rel >= 2)
		return -EINVAL; /* the gap */
	if (rel >= SDW_MAX_PDI)
		return -EINVAL;
	return rel;
}

static void pdi_init(struct sdw_link *l)
{
	struct sdw_ctx *c = l->ctx;
	uint16_t cap;
	int total;

	if (l->ace2)
		cap = mmio_rd16(l->shim, SHIM2_PCMSCAP);
	else
		cap = mmio_rd16(l->shim, SHIM_PCMSCAP(l->id));
	if (cap == 0xffff)
		cap = 0;
	l->pcm_in = (int)sdw_get_bits(cap, PCMSCAP_ISS);
	l->pcm_out = (int)sdw_get_bits(cap, PCMSCAP_OSS);
	l->pcm_bd = (int)sdw_get_bits(cap, PCMSCAP_BSS);
	total = l->pcm_bd + l->pcm_in + l->pcm_out;
	if (total > SDW_MAX_PDI)
		total = SDW_MAX_PDI;
	for (int i = 0; i < SDW_MAX_PDI; i++) {
		uint16_t chc = 0;
		if (i < total) {
			if (l->ace2)
				chc = mmio_rd16(l->shim, SHIM2_PCMSYCHC(i));
			else
				chc = mmio_rd16(l->shim, SHIM_PCMSYCHC(l->id, i));
			if (chc == 0xffff)
				chc = 0;
		}
		l->pdi_ch[i] = i < total ? (uint32_t)(chc & 0xf) + 1 : 0;
	}
	/* a well-known erratum: PDI2 of the first platforms reports a
	 * single channel but carries eight */
	if (!c->ace1 && !c->ace2 && total > 2 && l->pdi_ch[2] == 1)
		l->pdi_ch[2] = 8;
	l->pdi_busy = 0;
}

int sdw_intel_pdi_alloc(struct sdw_link *l, int dir, uint32_t ch, int *pdi,
			int *alh_id)
{
	int total, first = 2;

	if (!l || !pdi || !alh_id || !ch)
		return -EINVAL;
	total = l->pcm_bd + l->pcm_in + l->pcm_out;
	if (total > SDW_MAX_PDI)
		total = SDW_MAX_PDI;
	if (total <= first)
		first = 0;
	/* PDIs 0 and 1 carry bulk transfers; audio starts at 2 */
	for (int i = first; i < total; i++) {
		if (l->pdi_busy & (1u << i))
			continue;
		if (l->pdi_ch[i] && ch > l->pdi_ch[i])
			continue;
		l->pdi_busy |= 1u << i;
		*pdi = i;
		*alh_id = pdi_alh_id(l->id, i);
		(void)dir;
		return 0;
	}
	return -EBUSY;
}

void sdw_intel_pdi_config(struct sdw_link *l, int pdi, int dir, uint32_t ch,
			  int alh_id, int stream_tag)
{
	uint32_t v;
	uint16_t m;

	if (!l || !l->up || l->dead || pdi < 0 || pdi >= SDW_MAX_PDI || !ch ||
	    ch > 8)
		return;

	/* Cadence side: port direction, PDI -> port and channel mask */
	v = 0;
	if (dir == AUDIO_DIR_CAPTURE)
		v = CDNS_PORTCTRL_DIRN;
	else if (pdi == 0 || pdi == 1)
		v = CDNS_PORTCTRL_BULK_ENABLE;
	cdns_upd(l, CDNS_PORTCTRL(pdi),
		 CDNS_PORTCTRL_DIRN | CDNS_PORTCTRL_TEST_FAILED |
			 CDNS_PORTCTRL_BULK_ENABLE,
		 v);
	v = (pdi == 1) ? 0 : (uint32_t)pdi;
	v |= CDNS_PDI_CONFIG_SOFT_RESET;
	v |= ((1u << ch) - 1) << 8;
	cdns_wr(l, CDNS_PDI_CONFIG(pdi), v);

	/* Intel side: which gateway the PDI talks to */
	m = 0;
	if (dir == AUDIO_DIR_CAPTURE)
		m |= PCMSYCM_DIR;
	m = (uint16_t)sdw_set_bits(m, PCMSYCM_LCHN, 0);
	m = (uint16_t)sdw_set_bits(m, PCMSYCM_HCHN, ch - 1);
	if (l->ace2) {
		m = (uint16_t)sdw_set_bits(m, PCMSYCM_STREAM,
					   (uint32_t)stream_tag & 0x3f);
		mmio_wr16(l->shim, SHIM2_PCMSYCHM(pdi), m);
	} else {
		m = (uint16_t)sdw_set_bits(m, PCMSYCM_STREAM,
					   (uint32_t)alh_id & 0x3f);
		mmio_wr16(l->shim, SHIM_PCMSYCHM(l->id, pdi), m);
		if (l->alh && alh_id >= 0 && alh_id < ALH_NUM_STREAMS) {
			uint32_t a = mmio_rd32(l->alh, ALH_STRMZCFG(alh_id));
			a = sdw_set_bits(a, ALH_DMAT_MASK, ALH_DMAT_VAL);
			a = sdw_set_bits(a, ALH_CHN_MASK, ch - 1);
			mmio_wr32(l->alh, ALH_STRMZCFG(alh_id), a);
		}
	}
}

void sdw_intel_pdi_free(struct sdw_link *l, int pdi)
{
	if (!l || pdi < 0 || pdi >= SDW_MAX_PDI)
		return;
	if (l->up && !l->dead) {
		cdns_wr(l, CDNS_DPN_B0_CH_EN(pdi), 0);
		cdns_wr(l, CDNS_DPN_B0_CH_EN(pdi) + CDNS_DPN_B1_OFFSET, 0);
		if (l->ace2)
			mmio_wr16(l->shim, SHIM2_PCMSYCHM(pdi), 0);
	}
	l->pdi_busy &= ~(1u << pdi);
}

int sdw_intel_map_pdi_stream(struct sof_dev *sdev, int link, int dir,
			     int stream_tag)
{
	struct sdw_ctx *c = sdw_ctx_of(sdev);
	struct sdw_link *l;
	struct sdw_stream_rt *st;
	uint64_t fl;

	if (!c || link < 0 || link >= SDW_MAX_LINKS ||
	    (dir != AUDIO_DIR_PLAYBACK && dir != AUDIO_DIR_CAPTURE) ||
	    stream_tag < 0 || stream_tag > 63)
		return -EINVAL;
	l = &c->link[link];
	if (!l->up)
		return -ENODEV;
	spin_lock_irqsave(&l->stream_lock, &fl);
	st = NULL;
	for (int i = 0; i < SDW_LINK_STREAMS; i++) {
		struct sdw_stream_rt *x = &l->streams[i];
		if (x->state == SDW_STREAM_IDLE || x->pdi < 0 || x->dir != dir)
			continue;
		if (sdev->sdw_prep_function && x->fn &&
		    x->fn != sdev->sdw_prep_function)
			continue;
		st = x;
		break;
	}
	if (!st) {
		spin_unlock_irqrestore(&l->stream_lock, fl);
		return -ENOENT;
	}
	st->stream_tag = stream_tag;
	if (l->ace2)
		sdw_intel_pdi_config(l, st->pdi, dir, st->ch, st->alh_id,
				     stream_tag);
	spin_unlock_irqrestore(&l->stream_lock, fl);
	return 0;
}

/* ==========================================================================
 * Multi-link synchronisation
 * ========================================================================== */

void sdw_intel_sync_arm(struct sdw_link *l)
{
	struct sdw_ctx *c;

	if (!l || !l->multi_link || !l->up)
		return;
	c = l->ctx;
	if (l->ace2) {
		uint32_t v = mmio_rd32(c->hda, c->ml_sdw + ML_LSYNC);
		mmio_wr32(c->hda, c->ml_sdw + ML_LSYNC,
			  v | ML_LSYNC_CMDSYNC(l->id));
	} else {
		uint32_t v = mmio_rd32(l->shim, SHIM_SYNC);
		mmio_wr32(l->shim, SHIM_SYNC, v | SHIM_SYNC_CMDSYNC(l->id));
	}
}

int sdw_intel_sync_go(struct sdw_link *l)
{
	struct sdw_ctx *c;

	if (!l || !l->multi_link || !l->up)
		return 0;
	c = l->ctx;
	if (l->ace2) {
		uint32_t v = mmio_rd32(c->hda, c->ml_sdw + ML_LSYNC);
		if (v & (0xfu << 24))
			mmio_wr32(c->hda, c->ml_sdw + ML_LSYNC,
				  v | ML_LSYNC_SYNCGO);
	} else {
		uint32_t v = mmio_rd32(l->shim, SHIM_SYNC);
		if (v & SHIM_SYNC_CMDSYNC_MASK)
			mmio_wr32(l->shim, SHIM_SYNC, v | SHIM_SYNC_SYNCGO);
	}
	return 0;
}

/* ==========================================================================
 * Link power and glue
 * ========================================================================== */

static void pick_sync(uint32_t mclk, int have_mlcs, uint32_t *syncprd,
		      uint32_t *source)
{
	if (mclk % 6000000u) {
		if (mclk % 2400000u) {
			*syncprd = SYNCPRD_24_576;
			*source = MLCS_CARDINAL;
		} else {
			*syncprd = SYNCPRD_38_4;
			*source = MLCS_XTAL;
		}
	} else if (have_mlcs) {
		*syncprd = SYNCPRD_96;
		*source = MLCS_AUDIO_PLL;
	} else {
		*syncprd = SYNCPRD_24;
		*source = MLCS_XTAL;
	}
}

/* cAVS/ACE1: power all our links through the shared SHIM at once. */
static int shim_power_up(struct sdw_ctx *c, uint32_t mask)
{
	volatile uint8_t *shim = c->dsp + c->shim_off;
	uint32_t lcap, sync, lctl, syncprd, source, spa = 0, cpa = 0;
	uint32_t mclk = 0;
	int mlcs, tries;

	lcap = mmio_rd32(shim, SHIM_LCAP);
	if (lcap == 0xffffffffu)
		return -ENODEV;
	mlcs = (lcap & SHIM_LCAP_MLCS) ? 1 : 0;
	for (int i = 0; i < SDW_MAX_LINKS; i++) {
		if (mask & (1u << i)) {
			spa |= SHIM_LCTL_SPA(i);
			cpa |= SHIM_LCTL_CPA(i);
			if (!mclk)
				mclk = c->link[i].mprops.mclk_freq;
		}
	}
	if (!mclk)
		mclk = 19200000;
	pick_sync(mclk, mlcs, &syncprd, &source);
	if (source == MLCS_CARDINAL && !mlcs) {
		sdw_info("master clock %u Hz needs a clock source this SHIM lacks\n",
			 mclk);
		return -EINVAL;
	}

	/* the sync period first, with SYNCCPU so it takes */
	sync = mmio_rd32(shim, SHIM_SYNC);
	sync = sdw_set_bits(sync, SHIM_SYNC_SYNCPRD_MASK, syncprd);
	sync |= SHIM_SYNC_SYNCCPU;
	mmio_wr32(shim, SHIM_SYNC, sync);

	lctl = mmio_rd32(shim, SHIM_LCTL);
	mmio_wr32(shim, SHIM_LCTL, lctl | spa);
	for (tries = 0; tries < 200; tries++) {
		if ((mmio_rd32(shim, SHIM_LCTL) & cpa) == cpa)
			break;
		snd_udelay(50);
	}
	if (tries == 200) {
		sdw_info("links 0x%x did not power up (LCTL 0x%08x)\n", mask,
			 mmio_rd32(shim, SHIM_LCTL));
		return -ETIMEDOUT;
	}
	/* SYNCCPU clears once the links run */
	for (tries = 0; tries < 200; tries++) {
		if (!(mmio_rd32(shim, SHIM_SYNC) & SHIM_SYNC_SYNCCPU))
			break;
		snd_udelay(50);
	}
	if (tries == 200)
		sdw_info("sync generator did not start (SYNC 0x%08x)\n",
			 mmio_rd32(shim, SHIM_SYNC));
	if (mlcs) {
		lctl = mmio_rd32(shim, SHIM_LCTL);
		lctl = sdw_set_bits(lctl, SHIM_LCTL_MLCS_MASK, source);
		mmio_wr32(shim, SHIM_LCTL, lctl);
	}
	c->shim_up = 1;
	return 0;
}

static void shim_power_down(struct sdw_ctx *c, uint32_t mask)
{
	volatile uint8_t *shim = c->dsp + c->shim_off;
	uint32_t lctl, cpa = 0, spa = 0;

	for (int i = 0; i < SDW_MAX_LINKS; i++)
		if (mask & (1u << i)) {
			spa |= SHIM_LCTL_SPA(i);
			cpa |= SHIM_LCTL_CPA(i);
		}
	lctl = mmio_rd32(shim, SHIM_LCTL);
	if (lctl == 0xffffffffu)
		return;
	mmio_wr32(shim, SHIM_LCTL, lctl & ~spa);
	for (int t = 0; t < 200; t++) {
		if (!(mmio_rd32(shim, SHIM_LCTL) & cpa))
			break;
		snd_udelay(50);
	}
	c->shim_up = 0;
}

/* cAVS/ACE1: hand the I/O pins from the integration glue to the master */
static void shim_glue_to_master(struct sdw_link *l)
{
	volatile uint8_t *shim = l->shim;
	uint16_t io;

	io = mmio_rd16(shim, SHIM_IOCTL(l->id));
	io &= (uint16_t)~IOCTL_DOE;
	mmio_wr16(shim, SHIM_IOCTL(l->id), io);
	snd_udelay(15);
	io &= (uint16_t)~IOCTL_DO;
	mmio_wr16(shim, SHIM_IOCTL(l->id), io);
	snd_udelay(15);
	io |= IOCTL_MIF;
	mmio_wr16(shim, SHIM_IOCTL(l->id), io);
	snd_udelay(15);
	io &= (uint16_t)~(IOCTL_BKE | IOCTL_COE);
	mmio_wr16(shim, SHIM_IOCTL(l->id), io);
	snd_udelay(15);
}

static void shim_master_to_glue(struct sdw_link *l)
{
	volatile uint8_t *shim = l->shim;
	uint16_t io = mmio_rd16(shim, SHIM_IOCTL(l->id));

	if (io == 0xffff)
		return;
	io |= IOCTL_BKE | IOCTL_COE;
	mmio_wr16(shim, SHIM_IOCTL(l->id), io);
	snd_udelay(15);
	io &= (uint16_t)~IOCTL_MIF;
	mmio_wr16(shim, SHIM_IOCTL(l->id), io);
	snd_udelay(15);
}

static void shim_link_init(struct sdw_link *l)
{
	volatile uint8_t *shim = l->shim;
	uint16_t io = 0, act;

	/* keep the bus quiet while the glue owns the pins */
	io |= IOCTL_BKE;
	mmio_wr16(shim, SHIM_IOCTL(l->id), io);
	snd_udelay(15);
	io |= IOCTL_WPDD;
	mmio_wr16(shim, SHIM_IOCTL(l->id), io);
	snd_udelay(15);
	io |= IOCTL_DO;
	mmio_wr16(shim, SHIM_IOCTL(l->id), io);
	snd_udelay(15);
	io |= IOCTL_DOE;
	mmio_wr16(shim, SHIM_IOCTL(l->id), io);
	snd_udelay(15);

	shim_glue_to_master(l);

	act = mmio_rd16(shim, SHIM_CTMCTL(l->id));
	act = (uint16_t)sdw_set_bits(act, CTMCTL_DOAIS, 0x1);
	act |= CTMCTL_DACTQE | CTMCTL_DODS;
	mmio_wr16(shim, SHIM_CTMCTL(l->id), act);
	snd_udelay(15);
}

/* ACE2+: find the SoundWire extended link of the HDA multi-link cap */
static int ml_discover(struct sdw_ctx *c)
{
	struct sof_dev *sdev = c->sdev;
	uint32_t off, nl;
	int guard = 0;

	if (!c->hda || sdev->hda_size < 0x100)
		return -ENODEV;
	off = mmio_rd16(c->hda, HDA_LLCH);
	c->ml_cap = 0;
	while (off && off != 0xffff && guard++ < 16) {
		uint32_t h;
		if (off + 8 > sdev->hda_size || (off & 3))
			break;
		h = mmio_rd32(c->hda, off);
		if (h == 0xffffffffu)
			break;
		if (((h >> 16) & 0xfff) == HDA_CAP_ML) {
			c->ml_cap = off;
			break;
		}
		off = h & 0xffff;
	}
	if (!c->ml_cap)
		return -ENODEV;
	nl = (mmio_rd32(c->hda, c->ml_cap + ML_MLCD) & 0xf) + 1;
	for (uint32_t i = 0; i < nl && i < 16; i++) {
		uint32_t base = c->ml_cap + ML_LINK(i);
		uint32_t lcap, leptr;

		if (base + 0x40 > sdev->hda_size)
			break;
		lcap = mmio_rd32(c->hda, base + ML_LCAP);
		if (lcap == 0xffffffffu || !(lcap & ML_LCAP_ALT))
			continue;
		leptr = mmio_rd32(c->hda, base + ML_LEPTR);
		if (ML_LEPTR_ID(leptr) != ML_LEPTR_ID_SDW)
			continue;
		c->ml_sdw = base;
		c->ml_lcap = lcap;
		c->ml_slcount = (int)sdw_get_bits(lcap, ML_LCAP_SLCOUNT) + 1;
		c->ml_intc = (lcap & ML_LCAP_INTC) ? 1 : 0;
		c->ml_lss = (lcap & ML_LCAP_LSS) ? 1 : 0;
		c->ml_base = ML_LEPTR_PTR(leptr);
		if (!c->ml_base)
			c->ml_base = 0x30000;
		return 0;
	}
	return -ENODEV;
}

static int ml_link_power(struct sdw_link *l, int on)
{
	struct sdw_ctx *c = l->ctx;
	uint32_t lctl;
	int t;

	lctl = mmio_rd32(c->hda, c->ml_sdw + ML_LCTL);
	if (lctl == 0xffffffffu)
		return -ENODEV;
	if (on)
		lctl |= ML_LCTL_SPA(l->id);
	else
		lctl &= ~ML_LCTL_SPA(l->id);
	mmio_wr32(c->hda, c->ml_sdw + ML_LCTL, lctl);
	for (t = 0; t < 200; t++) {
		int cpa = (mmio_rd32(c->hda, c->ml_sdw + ML_LCTL) &
			   ML_LCTL_CPA(l->id)) ? 1 : 0;
		if (cpa == on)
			return 0;
		snd_udelay(20);
	}
	return -ETIMEDOUT;
}

static int ace2_link_power_up(struct sdw_link *l, int first)
{
	struct sdw_ctx *c = l->ctx;
	uint32_t syncprd, source, v;
	uint16_t act;
	int rc;

	pick_sync(l->mprops.mclk_freq, 1, &syncprd, &source);
	rc = ml_link_power(l, 1);
	if (rc) {
		sdw_info("link %d: sublink power-up failed (%d)\n", l->id, rc);
		return rc;
	}
	v = mmio_rd32(l->shim_vs, VS_LVSCTL);
	v = sdw_set_bits(v, VS_LVSCTL_MLCS, source);
	mmio_wr32(l->shim_vs, VS_LVSCTL, v);

	if (first && c->ml_lss) {
		v = mmio_rd32(c->hda, c->ml_sdw + ML_LSYNC);
		v = (v & ~ML_LSYNC_SYNCPRD) | (syncprd & ML_LSYNC_SYNCPRD);
		v |= ML_LSYNC_SYNCPU;
		mmio_wr32(c->hda, c->ml_sdw + ML_LSYNC, v);
		for (int t = 0; t < 200; t++) {
			if (!(mmio_rd32(c->hda, c->ml_sdw + ML_LSYNC) &
			      ML_LSYNC_SYNCPU))
				break;
			snd_udelay(20);
		}
	}
	if (first && c->ml_intc) {
		v = mmio_rd32(c->hda, c->ml_sdw + ML_LCTL);
		mmio_wr32(c->hda, c->ml_sdw + ML_LCTL, v | ML_LCTL_INTEN);
	}

	/* timing controls of the vendor SHIM */
	act = mmio_rd16(l->shim_vs, VS_ACTMCTL);
	act = (uint16_t)sdw_set_bits(act, VS_ACT_DOAISE, l->mprops.doaise);
	act = (uint16_t)sdw_set_bits(act, VS_ACT_DOAIS, l->mprops.doais);
	act = (uint16_t)sdw_set_bits(act, VS_ACT_DODSE, l->mprops.dodse);
	act = (uint16_t)sdw_set_bits(act, VS_ACT_DODS, l->mprops.dods);
	if (c->ace3) {
		act = (uint16_t)sdw_set_bits(act, VS_ACT_CLDE, l->mprops.clde);
		act = (uint16_t)sdw_set_bits(act, VS_ACT_DOAISE2,
					     l->mprops.doaise2);
		act = (uint16_t)sdw_set_bits(act, VS_ACT_DODSE2,
					     l->mprops.dodse2);
		act = (uint16_t)sdw_set_bits(act, VS_ACT_CLDS, l->mprops.clds);
		act = (uint16_t)sdw_set_bits(act, VS_ACT_CLSS, l->mprops.clss);
	}
	act |= VS_ACT_DACTQE;
	mmio_wr16(l->shim_vs, VS_ACTMCTL, act);
	snd_udelay(15);
	return 0;
}

/* ==========================================================================
 * Cadence bring-up
 * ========================================================================== */

static void cdns_check_self_clearing(struct sdw_link *l, const char *what)
{
	uint32_t ipc = cdns_ip_rd(l, CDNS_IP_MCP_CONTROL);
	uint32_t ctl = cdns_rd(l, CDNS_MCP_CONTROL);
	uint32_t upd = cdns_rd(l, CDNS_MCP_CONFIG_UPDATE);

	if (ipc & CDNS_IP_MCP_CONTROL_SW_RST)
		sdw_info("link %d %s: SW_RST stuck\n", l->id, what);
	if (ctl & (CDNS_MCP_CONTROL_CMD_RST | CDNS_MCP_CONTROL_SOFT_RST |
		   CDNS_MCP_CONTROL_CLK_STOP_CLR))
		sdw_info("link %d %s: MCP_CONTROL 0x%08x not cleared\n", l->id,
			 what, ctl);
	if (upd & CDNS_MCP_CONFIG_UPDATE_BIT)
		sdw_info("link %d %s: CONFIG_UPDATE stuck\n", l->id, what);
}

static int cdns_soft_reset(struct sdw_link *l)
{
	int rc;

	cdns_upd(l, CDNS_MCP_CONTROL, CDNS_MCP_CONTROL_SOFT_RST,
		 CDNS_MCP_CONTROL_SOFT_RST);
	rc = cdns_config_update(l);
	if (rc)
		return rc;
	return cdns_wait(l, CDNS_MCP_CONTROL, CDNS_MCP_CONTROL_SOFT_RST, 0, 40);
}

static int cdns_init(struct sdw_link *l)
{
	struct sdw_bus_params *bp = &l->params;
	uint32_t div, v, ssp;
	int r, c;

	if (!l->mprops.default_frame_rate || !l->params.row)
		return -EINVAL;
	/* clock divider, both banks */
	div = l->mprops.mclk_freq * CDNS_DOUBLE_RATE / bp->curr_dr_freq;
	if (div)
		div--;
	if (div > 0xff)
		div = 0xff;
	cdns_upd(l, CDNS_MCP_CLK_CTRL0, CDNS_MCP_CLK_MCLKD_MASK, div);
	cdns_upd(l, CDNS_MCP_CLK_CTRL1, CDNS_MCP_CLK_MCLKD_MASK, div);

	r = sdw_find_row_index(bp->row);
	c = sdw_find_col_index(bp->col);
	if (r < 0)
		r = 1; /* 50 rows */
	if (c < 0)
		c = 1; /* 4 columns */
	cdns_wr(l, CDNS_MCP_FRAME_SHAPE_INIT, ((uint32_t)r << 3) | (uint32_t)c);

	ssp = l->mprops.default_frame_rate / (GSYNC_KHZ * 1000u);
	if (!ssp)
		ssp = 1;
	cdns_wr(l, CDNS_MCP_SSP_CTRL0, ssp);
	cdns_wr(l, CDNS_MCP_SSP_CTRL1, ssp);

	cdns_check_self_clearing(l, "init");
	l->msg_count = cdns_rd(l, CDNS_MCP_FIFOLEVEL);

	/* flush the command interface, accept commands, allow wakes */
	cdns_upd(l, CDNS_MCP_CONTROL, CDNS_MCP_CONTROL_CMD_RST,
		 CDNS_MCP_CONTROL_CMD_RST);
	cdns_ip_upd(l, CDNS_IP_MCP_CONTROL, CDNS_IP_MCP_CONTROL_CMD_ACCEPT,
		    CDNS_IP_MCP_CONTROL_CMD_ACCEPT);
	cdns_ip_upd(l, CDNS_IP_MCP_CONTROL, CDNS_IP_MCP_CONTROL_BLOCK_WAKEUP,
		    0);

	v = cdns_rd(l, CDNS_MCP_CONFIG);
	v &= ~CDNS_MCP_CONFIG_BUS_REL;
	cdns_wr(l, CDNS_MCP_CONFIG, v);

	v = cdns_ip_rd(l, CDNS_IP_MCP_CONFIG);
	v &= ~(CDNS_IP_MCP_CONFIG_OP | CDNS_IP_MCP_CONFIG_CMD |
	       CDNS_IP_MCP_CONFIG_SNIFFER);
	if (l->multi_link)
		v |= CDNS_IP_MCP_CONFIG_MMASTER;
	else
		v &= ~CDNS_IP_MCP_CONFIG_MMASTER;
	cdns_ip_wr(l, CDNS_IP_MCP_CONFIG, v);
	return 0;
}

static void cdns_enable_interrupts(struct sdw_link *l, int on)
{
	uint32_t mask = 0;
	uint64_t fl;

	if (on) {
		uint32_t s;
		/* drop stale peripheral status first */
		s = cdns_rd(l, CDNS_MCP_SLAVE_INTSTAT0);
		cdns_wr(l, CDNS_MCP_SLAVE_INTSTAT0, s);
		s = cdns_rd(l, CDNS_MCP_SLAVE_INTSTAT1);
		cdns_wr(l, CDNS_MCP_SLAVE_INTSTAT1, s);
		/* status changes and errors; the response watermark stays
		 * masked: transfers are polled */
		mask = CDNS_MCP_INT_SLAVE_MASK | CDNS_MCP_INT_CTRL_CLASH |
		       CDNS_MCP_INT_DATA_CLASH | CDNS_MCP_INT_PARITY |
		       CDNS_MCP_INT_IRQ;
	}
	spin_lock_irqsave(&l->irq_lock, &fl);
	cdns_wr(l, CDNS_MCP_SLAVE_INTMASK0, on ? 0xffffffffu : 0);
	cdns_wr(l, CDNS_MCP_SLAVE_INTMASK1, on ? 0x0000ffffu : 0);
	cdns_wr(l, CDNS_MCP_INTMASK, mask);
	spin_unlock_irqrestore(&l->irq_lock, fl);
}

static int cdns_exit_reset(struct sdw_link *l)
{
	cdns_upd(l, CDNS_MCP_CONTROL, CDNS_MCP_CONTROL_HW_RST,
		 CDNS_MCP_CONTROL_HW_RST);
	return cdns_config_update(l);
}

static int cdns_start(struct sdw_link *l)
{
	int rc;

	rc = cdns_soft_reset(l);
	if (rc) {
		sdw_info("link %d: soft reset failed (%d)\n", l->id, rc);
		return rc;
	}
	sdw_intel_sync_arm(l);
	rc = cdns_init(l);
	if (rc)
		return rc;
	cdns_wr(l, CDNS_MCP_CONFIG_UPDATE, CDNS_MCP_CONFIG_UPDATE_BIT);
	sdw_intel_sync_go(l);
	rc = cdns_wait(l, CDNS_MCP_CONFIG_UPDATE, CDNS_MCP_CONFIG_UPDATE_BIT, 0,
		       40);
	if (rc) {
		sdw_info("link %d: configuration update timed out\n", l->id);
		return rc;
	}
	cdns_enable_interrupts(l, 1);
	rc = cdns_exit_reset(l);
	if (rc) {
		sdw_info("link %d: leaving reset failed (%d)\n", l->id, rc);
		return rc;
	}
	/* the reset sequence on the bus takes a few frames */
	snd_mdelay(1);
	for (int i = 0; i < CDNS_RESET_ITERATIONS; i++) {
		if (!(cdns_rd(l, CDNS_MCP_CONTROL) & CDNS_MCP_CONTROL_HW_RST))
			break;
		snd_mdelay(1);
	}
	cdns_check_self_clearing(l, "start");
	l->params.curr_bank = 0;
	l->params.next_bank = 1;
	return 0;
}

static void cdns_stop(struct sdw_link *l)
{
	if (!l->up)
		return;
	cdns_enable_interrupts(l, 0);
	l->up = 0;
}

/* ==========================================================================
 * DSP-level interrupt routing
 * ========================================================================== */

static void dsp_sdw_irq_enable(struct sdw_ctx *c, int on)
{
	struct sof_dev *sdev = c->sdev;

	if (c->ace2) {
		if (c->ml_sdw && c->ml_intc) {
			uint32_t v = mmio_rd32(c->hda, c->ml_sdw + ML_LCTL);
			if (v != 0xffffffffu)
				mmio_wr32(c->hda, c->ml_sdw + ML_LCTL,
					  on ? (v | ML_LCTL_INTEN) :
					       (v & ~ML_LCTL_INTEN));
		}
	} else if (c->ace1) {
		uint32_t ptr = mmio_rd32(c->dsp, MTL_HFINTIPPTR) &
			       MTL_HFINTIPPTR_MASK;
		uint32_t v;
		if (ptr && ptr + 4 <= sdev->dsp_size) {
			v = mmio_rd32(c->dsp, ptr);
			if (v != 0xffffffffu)
				mmio_wr32(c->dsp, ptr,
					  on ? (v | MTL_IRQ_SNDW) :
					       (v & ~MTL_IRQ_SNDW));
		}
		if (MTL_HFSNDWIE + 4 <= sdev->dsp_size) {
			v = mmio_rd32(c->dsp, MTL_HFSNDWIE);
			if (v != 0xffffffffu)
				mmio_wr32(c->dsp, MTL_HFSNDWIE,
					  on ? (v | MTL_HFSNDWIE_MASK) :
					       (v & ~MTL_HFSNDWIE_MASK));
		}
	} else {
		uint32_t v = mmio_rd32(c->dsp, CAVS_ADSPIC2);
		if (v != 0xffffffffu)
			mmio_wr32(c->dsp, CAVS_ADSPIC2,
				  on ? (v | CAVS_ADSPIx2_SNDW) :
				       (v & ~CAVS_ADSPIx2_SNDW));
	}
	c->irq_enabled = on;
}

void sdw_intel_irq(struct sof_dev *sdev)
{
	struct sdw_ctx *c = sdw_ctx_of(sdev);

	if (!c || c->stopping)
		return;
	for (int i = 0; i < SDW_MAX_LINKS; i++) {
		struct sdw_link *l = &c->link[i];
		uint32_t st, handled = 0;
		uint64_t fl;

		if (!l->up || l->dead)
			continue;
		st = cdns_rd(l, CDNS_MCP_INTSTAT);
		if (!st || st == 0xffffffffu)
			continue;
		if (st & CDNS_MCP_INT_RESERVED)
			continue; /* not a state the IP can be in: ignore */
		if (!(st & CDNS_MCP_INT_IRQ))
			continue;
		l->st_irqs++;
		if (st & CDNS_MCP_INT_PARITY) {
			l->st_parity++;
			handled |= CDNS_MCP_INT_PARITY;
		}
		if (st & CDNS_MCP_INT_CTRL_CLASH) {
			l->st_ctrl_clash++;
			handled |= CDNS_MCP_INT_CTRL_CLASH;
		}
		if (st & CDNS_MCP_INT_DATA_CLASH) {
			l->st_data_clash++;
			handled |= CDNS_MCP_INT_DATA_CLASH;
		}
		if (st & CDNS_MCP_INT_CMD_ERR) {
			l->st_cmd_err++;
			handled |= CDNS_MCP_INT_CMD_ERR;
		}
		if (st & CDNS_MCP_INT_DPINT) {
			uint32_t p = cdns_rd(l, CDNS_MCP_PORT_INTSTAT);
			cdns_wr(l, CDNS_MCP_PORT_INTSTAT, p);
			handled |= CDNS_MCP_INT_DPINT;
		}
		if (st & CDNS_MCP_INT_SLAVE_MASK) {
			/* mask further status interrupts until the work ran;
			 * it acknowledges and unmasks */
			spin_lock_irqsave(&l->irq_lock, &fl);
			cdns_upd(l, CDNS_MCP_INTMASK, CDNS_MCP_INT_SLAVE_MASK,
				 0);
			spin_unlock_irqrestore(&l->irq_lock, fl);
			l->irq_pending = 1;
			sdw_bus_irq_notify(l);
		}
		/* never acknowledge RX_WL here: transfers poll it */
		handled |= st & (CDNS_MCP_INT_WAKEUP | CDNS_MCP_INT_TXE |
				 CDNS_MCP_INT_TXF | CDNS_MCP_INT_RX_NE);
		if (handled)
			cdns_wr(l, CDNS_MCP_INTSTAT, handled);
	}
}

/* ==========================================================================
 * Init / exit
 * ========================================================================== */

const char *sdw_intel_gen_name(const struct sdw_ctx *c)
{
	if (!c)
		return "?";
	if (c->ace3)
		return "ACE 3.x";
	if (c->ace2)
		return "ACE 2.x";
	if (c->ace1)
		return "ACE 1.x";
	switch (c->platform) {
	case SOF_PLAT_TGL:
		return "cAVS 2.5";
	case SOF_PLAT_ICL:
		return "cAVS 2.0";
	case SOF_PLAT_CNL:
		return "cAVS 1.8";
	default:
		return "cAVS";
	}
}

static void ctx_generation(struct sdw_ctx *c)
{
	struct sof_dev *sdev = c->sdev;
	uint16_t dev = sdev->pci ? sdev->pci->device_id : 0;

	c->platform = sdev->platform;
	switch (dev) {
	case 0x7e28: /* Meteor Lake */
	case 0x7728: /* Arrow Lake */
	case 0x7f50:
		c->platform = SOF_PLAT_MTL;
		break;
	case 0xa828: /* Lunar Lake */
		c->platform = SOF_PLAT_LNL;
		break;
	case 0xe428: /* Panther Lake */
	case 0xe328:
	case 0x4d28:
		c->platform = SOF_PLAT_PTL;
		break;
	default:
		break;
	}
	c->ace1 = c->platform == SOF_PLAT_MTL;
	c->ace2 = c->platform >= SOF_PLAT_LNL;
	c->ace3 = c->platform >= SOF_PLAT_PTL;
	if (c->ace1) {
		c->shim_off = SHIM_BASE_ACE1;
		c->alh_off = ALH_BASE_ACE1;
	} else {
		c->shim_off = SHIM_BASE_CAVS;
		c->alh_off = ALH_BASE_CAVS;
	}
}

static int link_map(struct sdw_ctx *c, struct sdw_link *l)
{
	struct sof_dev *sdev = c->sdev;

	if (c->ace2) {
		uint32_t base = c->ml_base + SHIM2_INSTANCE * (uint32_t)l->id;
		if (!c->hda || base + SHIM2_VS_OFF + 0x100 > sdev->hda_size)
			return -ERANGE;
		l->shim = c->hda + base;
		l->regs = c->hda + base + SHIM2_IP_OFF;
		l->shim_vs = c->hda + base + SHIM2_VS_OFF;
		l->ip_offset = CADENCE_IP_OFFSET;
		l->alh = NULL;
		l->ace2 = 1;
		if (base + SHIM2_IP_OFF + CADENCE_IP_OFFSET + 0x100 >
		    sdev->hda_size)
			return -ERANGE;
	} else {
		uint32_t base = LINK_BASE + LINK_SIZE * (uint32_t)l->id;
		if (base + 0x2000 > sdev->dsp_size ||
		    c->shim_off + 0x200 > sdev->dsp_size ||
		    c->alh_off + 4 * ALH_NUM_STREAMS > sdev->dsp_size)
			return -ERANGE;
		l->regs = c->dsp + base;
		l->shim = c->dsp + c->shim_off;
		l->alh = c->dsp + c->alh_off;
		l->shim_vs = NULL;
		l->ip_offset = 0;
		l->ace2 = 0;
	}
	return 0;
}

int sdw_intel_init(struct sof_dev *sdev)
{
	struct sdw_ctx *c;
	uint32_t want, lcount, up_mask = 0;
	int first = 1;

	if (!sdev)
		return -EINVAL;
	want = sdev->machine.sdw_link_mask & ((1u << SDW_MAX_LINKS) - 1);
	if (!want)
		return 0; /* nothing on SoundWire */
	if (sdev->sdw)
		return 0; /* already up */
	if (!sdev->dsp || !sdev->dsp_size) {
		sdw_info("no DSP registers mapped, SoundWire unavailable\n");
		return -ENODEV;
	}

	c = kcalloc(1, sizeof(*c));
	if (!c)
		return -ENOMEM;
	c->sdev = sdev;
	c->dsp = sdev->dsp;
	c->hda = sdev->hda;
	ctx_generation(c);
	snd_mutex_init(&c->ctl_lock, "sdw_ctl");
	c->out_mode = AUDIO_OUTSEL_AUTO;
	for (int i = 0; i <= AUDIO_CTL_MAX; i++) {
		c->vol[i][0] = c->vol[i][1] = 75;
		c->mute[i] = 0;
	}
	c->vol[AUDIO_CTL_MASTER][0] = c->vol[AUDIO_CTL_MASTER][1] = 80;

	if (c->ace2) {
		if (ml_discover(c)) {
			sdw_info("no SoundWire extended link in the HDA capabilities\n");
			kfree(c);
			return -ENODEV;
		}
		lcount = (uint32_t)c->ml_slcount;
	} else {
		uint32_t lcap = mmio_rd32(c->dsp, c->shim_off + SHIM_LCAP);
		if (lcap == 0xffffffffu) {
			sdw_info("SoundWire SHIM reads all ones\n");
			kfree(c);
			return -ENODEV;
		}
		lcount = lcap & SHIM_LCAP_LCOUNT;
	}
	c->hw_lcount = lcount;
	if (lcount < SDW_MAX_LINKS)
		want &= (1u << lcount) - 1;
	if (!want) {
		sdw_info("hardware has %u links, none of the described ones\n",
			 lcount);
		kfree(c);
		return -ENODEV;
	}
	sdev->sdw = c;

	for (int i = 0; i < SDW_MAX_LINKS; i++) {
		struct sdw_link *l = &c->link[i];

		l->sdev = sdev;
		l->ctx = c;
		l->id = i;
		spinlock_init(&l->bus_lock, "sdw_bus");
		spinlock_init(&l->stream_lock, "sdw_stream");
		spinlock_init(&l->irq_lock, "sdw_irq");
		snd_mutex_init(&l->service_lock, "sdw_service");
		for (int k = 0; k < SDW_LINK_STREAMS; k++) {
			l->streams[k].pdi = -1;
			l->streams[k].alh_id = -1;
			l->streams[k].deferred_on = -1;
		}
		l->present = (sdev->machine.sdw_link_mask >> i) & 1;
		if (!(want & (1u << i)))
			continue;
		sof_acpi_master_props(sdev, i, &l->mprops);
		if (!l->mprops.mclk_freq)
			l->mprops.mclk_freq = 19200000;
		if (link_map(c, l)) {
			sdw_info("link %d: registers outside the BAR\n", i);
			continue;
		}
		l->enabled = 1;
		c->link_mask |= 1u << i;
		sdw_bus_link_setup(l);
	}
	if (!c->link_mask) {
		sdev->sdw = NULL;
		kfree(c);
		return -ENODEV;
	}

	/* power */
	if (!c->ace2) {
		if (shim_power_up(c, c->link_mask)) {
			sdw_info("link power-up failed\n");
			for (int i = 0; i < SDW_MAX_LINKS; i++)
				if (c->link[i].enabled)
					sdw_bus_link_teardown(&c->link[i]);
			sdev->sdw = NULL;
			kfree(c);
			return -EIO;
		}
	}
	for (int i = 0; i < SDW_MAX_LINKS; i++) {
		struct sdw_link *l = &c->link[i];

		if (!l->enabled)
			continue;
		if (c->ace2) {
			if (ace2_link_power_up(l, first)) {
				l->enabled = 0;
				continue;
			}
			first = 0;
		} else {
			shim_link_init(l);
		}
		l->powered = 1;
		if (cdns_start(l)) {
			sdw_info("link %d failed to start\n", i);
			l->enabled = 0;
			continue;
		}
		pdi_init(l);
		l->up = 1;
		up_mask |= 1u << i;
	}
	if (!up_mask) {
		sdw_info("no SoundWire link came up\n");
		sdw_intel_exit(sdev);
		return -EIO;
	}
	dsp_sdw_irq_enable(c, 1);
	sdw_info("%s, links 0x%x up (hardware has %u)\n", sdw_intel_gen_name(c),
		 up_mask, lcount);

	/* enumerate: peripherals attach within a few frames of the reset */
	for (int i = 0; i < SDW_MAX_LINKS; i++)
		if (c->link[i].up)
			sdw_bus_link_started(&c->link[i]);
	{
		uint64_t deadline = hrtimer_now_ns() + 1500ull * 1000000ull;
		for (;;) {
			int missing = 0;
			for (int i = 0; i < SDW_MAX_LINKS; i++) {
				struct sdw_link *l = &c->link[i];
				if (!l->up || l->dead)
					continue;
				sdw_bus_service(l);
				for (int s = 0; s < l->nslaves; s++)
					if (l->slaves[s].status ==
					    SDW_SLAVE_UNATTACHED)
						missing++;
			}
			if (!missing || hrtimer_now_ns() >= deadline)
				break;
			sdw_msleep(20);
		}
	}
	sdw_bus_init_pending(c);
	sdw_bus_start_polling(c);
	return 0;
}

void sdw_intel_exit(struct sof_dev *sdev)
{
	struct sdw_ctx *c = sdw_ctx_of(sdev);
	uint32_t mask = 0;

	if (!c)
		return;
	c->stopping = 1;
	dsp_sdw_irq_enable(c, 0);
	for (int i = 0; i < SDW_MAX_LINKS; i++) {
		struct sdw_link *l = &c->link[i];

		if (!l->enabled && !l->up)
			continue;
		sdw_bus_link_teardown(l);
		cdns_stop(l);
		if (l->powered) {
			if (l->ace2)
				ml_link_power(l, 0);
			else
				shim_master_to_glue(l);
			mask |= 1u << i;
			l->powered = 0;
		}
	}
	if (!c->ace2 && mask && c->shim_up)
		shim_power_down(c, mask);
	/* Work items may still hold the context: wait for them to drain
	 * (they see `stopping') before freeing; give up and leak after a
	 * bounded time rather than free memory still in use. */
	for (int t = 0; t < 200; t++) {
		int busy = 0;
		for (int i = 0; i < SDW_MAX_LINKS; i++)
			busy |= c->link[i].work_queued;
		if (!busy && !c->poll_armed) {
			sdev->sdw = NULL;
			kfree(c);
			return;
		}
		sdw_msleep(10);
	}
	sdev->sdw = NULL;
	sdw_info("SoundWire work still pending at exit; state kept\n");
}

/* ==========================================================================
 * Register dump
 * ========================================================================== */

long sdw_intel_dump(struct sdw_ctx *c, char *buf, size_t len)
{
	size_t pos = 0;

	if (!c || !buf || !len)
		return 0;
	pos = sdw_dump_printf(buf, len, pos,
			      "SoundWire master: %s, %u hardware links, mask 0x%x\n",
			      sdw_intel_gen_name(c), c->hw_lcount, c->link_mask);
	if (!c->ace2) {
		volatile uint8_t *shim = c->dsp + c->shim_off;
		pos = sdw_dump_printf(buf, len, pos,
				      "  SHIM LCAP 0x%08x LCTL 0x%08x SYNC 0x%08x\n",
				      mmio_rd32(shim, SHIM_LCAP),
				      mmio_rd32(shim, SHIM_LCTL),
				      mmio_rd32(shim, SHIM_SYNC));
	} else if (c->ml_sdw) {
		pos = sdw_dump_printf(buf, len, pos,
				      "  ML LCAP 0x%08x LCTL 0x%08x LSYNC 0x%08x base 0x%x\n",
				      mmio_rd32(c->hda, c->ml_sdw + ML_LCAP),
				      mmio_rd32(c->hda, c->ml_sdw + ML_LCTL),
				      mmio_rd32(c->hda, c->ml_sdw + ML_LSYNC),
				      c->ml_base);
	}
	for (int i = 0; i < SDW_MAX_LINKS; i++) {
		struct sdw_link *l = &c->link[i];

		if (!l->enabled && !l->present)
			continue;
		pos = sdw_dump_printf(buf, len, pos,
				      "  link %d: %s%s%s mclk %u bus %u Hz frame %ux%u bank %d\n",
				      i, l->up ? "up" : "down",
				      l->dead ? " DEAD" : "",
				      l->ace2 ? " (sublink)" : "",
				      l->mprops.mclk_freq,
				      l->params.curr_dr_freq / 2, l->params.row,
				      l->params.col, l->params.curr_bank);
		if (!l->up)
			continue;
		pos = sdw_dump_printf(buf, len, pos,
				      "    MCP CONFIG 0x%08x CONTROL 0x%08x IP_CONFIG 0x%08x IP_CONTROL 0x%08x\n",
				      cdns_rd(l, CDNS_MCP_CONFIG),
				      cdns_rd(l, CDNS_MCP_CONTROL),
				      cdns_ip_rd(l, CDNS_IP_MCP_CONFIG),
				      cdns_ip_rd(l, CDNS_IP_MCP_CONTROL));
		pos = sdw_dump_printf(buf, len, pos,
				      "    STAT 0x%08x INTSTAT 0x%08x INTMASK 0x%08x SLAVE_STAT 0x%08x\n",
				      cdns_rd(l, CDNS_MCP_STAT),
				      cdns_rd(l, CDNS_MCP_INTSTAT),
				      cdns_rd(l, CDNS_MCP_INTMASK),
				      cdns_rd(l, CDNS_MCP_SLAVE_STAT));
		pos = sdw_dump_printf(buf, len, pos,
				      "    FRAME_SHAPE_INIT 0x%x CLK_CTRL 0x%x/0x%x SSP 0x%x FIFO 0x%x/0x%x\n",
				      cdns_rd(l, CDNS_MCP_FRAME_SHAPE_INIT),
				      cdns_rd(l, CDNS_MCP_CLK_CTRL0),
				      cdns_rd(l, CDNS_MCP_CLK_CTRL1),
				      cdns_rd(l, CDNS_MCP_SSP_CTRL0),
				      cdns_rd(l, CDNS_MCP_FIFOLEVEL),
				      cdns_rd(l, CDNS_MCP_FIFOSTAT));
		pos = sdw_dump_printf(buf, len, pos,
				      "    PDIs: %d bidir, %d in, %d out, busy 0x%x\n",
				      l->pcm_bd, l->pcm_in, l->pcm_out,
				      l->pdi_busy);
		for (int p = 2; p < l->pcm_bd + l->pcm_in + l->pcm_out &&
				p < SDW_MAX_PDI;
		     p++) {
			if (!(l->pdi_busy & (1u << p)))
				continue;
			pos = sdw_dump_printf(buf, len, pos,
					      "    PDI%d: cfg 0x%08x portctrl 0x%08x dpn0 0x%08x/0x%08x dpn1 0x%08x/0x%08x\n",
					      p, cdns_rd(l, CDNS_PDI_CONFIG(p)),
					      cdns_rd(l, CDNS_PORTCTRL(p)),
					      cdns_rd(l, CDNS_DPN_B0_CONFIG(p)),
					      cdns_rd(l, CDNS_DPN_B0_CH_EN(p)),
					      cdns_rd(l, CDNS_DPN_B0_CONFIG(p) +
							       CDNS_DPN_B1_OFFSET),
					      cdns_rd(l, CDNS_DPN_B0_CH_EN(p) +
							       CDNS_DPN_B1_OFFSET));
		}
		pos = sdw_dump_printf(buf, len, pos,
				      "    cmds %u ignored %u nak %u timeouts %u irqs %u parity %u clash %u/%u cmd-err %u bank switches %u\n",
				      l->st_cmds, l->st_ignored, l->st_nak,
				      l->st_timeout, l->st_irqs, l->st_parity,
				      l->st_ctrl_clash, l->st_data_clash,
				      l->st_cmd_err, l->st_bank_switch);
	}
	return (long)pos;
}
