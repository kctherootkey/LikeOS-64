// LikeOS -- the DVO ports of the gen2 parts and their encoder chips.
//
// The i830, i845G, i85x and i865G have no TMDS or LVDS transmitter of
// their own beyond the i85x's LVDS port; digital panels and monitors hang
// off DVO, a 12-bit double-pumped parallel pixel bus (DVOA on the i830,
// DVOB and DVOC on all of them) that feeds an external encoder chip on the
// motherboard.  The port register only selects the pipe, the sync
// polarities and the data order; everything else -- the TMDS link, the
// LVDS serialiser, the panel scaler and power sequencing -- lives in the
// chip, which is programmed over a GPIO I2C pair.  The chips seen in the
// field are the Chrontel CH7xxx and Silicon Image SiI164 and TI TFP410 DVI
// transmitters, the Chrontel CH7017 LVDS encoder, the Intel 82807AA "IVCH"
// LVDS controller of i830 laptops and the National Semiconductor NS2501
// LVDS scaler.  This file probes the buses for one of them and drives the
// port and the chip through a mode set.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2006-2007 Intel Corporation
// Portions Copyright (C) 2006 Dave Airlie
// Portions Copyright (C) 2006 Eric Anholt
// Portions Copyright (C) 2007 Dave Mueller
// Portions Copyright (C) 2012 Gilles Dartiguelongue, Thomas Richter

#include <kernel/dev/gpu/i915/intel_legacy.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

#define DVO_ARRAY_SIZE(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* The active data order bit of the port register (kept as found). */
#define LG_DVO_ACT_DATA_ORDER_MASK (1u << 6)

/* What kind of chip a table entry is. */
#define DVO_CHIP_LVDS 1
#define DVO_CHIP_TMDS 2
#define DVO_CHIP_LVDS_NO_FIXED 5

/* Why a chip refuses a mode (0 = it takes it). */
#define DVO_MODE_OK 0
#define DVO_MODE_CLOCK_HIGH 1
#define DVO_MODE_ONE_SIZE 2
#define DVO_MODE_PANEL 3

struct dvo_dev;

/* What every encoder chip offers. */
struct dvo_chip_ops {
	/* Is the chip there?  1 when it answered with its ids. */
	int (*init)(struct dvo_dev *dvo);
	/* Output on or off; the chips have no intermediate power levels. */
	void (*dpms)(struct dvo_dev *dvo, int enable);
	/* Chip-specific limits only; DVO_MODE_OK or a reason. */
	int (*mode_valid)(struct dvo_dev *dvo, const struct drm_mode_modeinfo *mode);
	/* Program `mode' (what the client asked for) as sent in `adj' (what
	 * the pipe runs).  Called with the output off. */
	void (*mode_set)(struct dvo_dev *dvo, const struct drm_mode_modeinfo *mode,
			 const struct drm_mode_modeinfo *adj);
	/* 1 when a sink is attached. */
	int (*detect)(struct dvo_dev *dvo);
	/* 1 when the chip's output is on. */
	int (*get_hw_state)(struct dvo_dev *dvo);
	void (*dump_regs)(struct dvo_dev *dvo);
};

/* One place an encoder chip may sit. */
struct dvo_device_desc {
	int type; /* DVO_CHIP_* */
	const char *name;
	int port; /* LG_PORT_A..C: DVOA..DVOC */
	unsigned gpio; /* the control bus when not the default, else 0 */
	uint8_t addr; /* 7-bit I2C address */
	const struct dvo_chip_ops *ops;
};

/* ---- the NS2501's mode table (needed by the per-output state) -------------------- */

struct ns2501_configuration {
	uint8_t sync; /* register C0 */
	uint8_t conf; /* register 08 */
	uint8_t syncb; /* register 41 */
	uint8_t dither; /* register F9 */
	uint8_t pll_a; /* register 1B */
	uint16_t pll_b; /* registers 1C/1D */
	uint16_t hstart; /* C1/C2 */
	uint16_t hstop; /* C3/C4 */
	uint16_t vstart; /* C5/C6 */
	uint16_t vstop; /* C7/C8 */
	uint16_t vsync; /* 80/81 */
	uint16_t vtotal; /* 82/83 */
	uint16_t hpos; /* 98/99 */
	uint16_t vpos; /* 8E/8F */
	uint16_t voffs; /* 9C/9D */
	uint16_t hscale; /* B8/B9 */
	uint16_t vscale; /* 10/11 */
};

/* The IVCH registers that some firmware forgets over a suspend and that
 * are therefore written back before every mode set and power change.
 * VR10 must come last. */
static const uint16_t ivch_backup_addresses[] = {
	0x11, 0x12,
	0x18, 0x19, 0x1a, 0x1f,
	0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
	0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
	0x8e, 0x8f,
	0x10
};

#define IVCH_NUM_BACKUP DVO_ARRAY_SIZE(ivch_backup_addresses)

/* The per-output state: which chip, on which bus. */
struct dvo_dev {
	struct lg_display *d;
	const struct dvo_device_desc *desc;
	unsigned pin; /* the control bus's GMBUS pin */
	struct i2c_adapter *i2c;
	uint32_t gpio_reg; /* the pin's GPIO pair (IVCH bit-banging) */
	int quiet; /* no failure messages while probing */
	/* IVCH */
	uint16_t ivch_width, ivch_height;
	uint16_t ivch_backup[IVCH_NUM_BACKUP];
	/* NS2501 */
	const struct ns2501_configuration *ns_conf;
};

/* ---- plain 8-bit register access over the adapter ------------------------------ */

static int dvo_readb(struct dvo_dev *dvo, uint8_t reg, uint8_t *val)
{
	uint8_t out_buf[2] = { reg, 0 };
	uint8_t in_buf[2] = { 0, 0 };
	struct i2c_msg msgs[2] = {
		{ .addr = dvo->desc->addr, .flags = 0, .len = 1, .buf = out_buf },
		{ .addr = dvo->desc->addr, .flags = I2C_M_RD, .len = 1, .buf = in_buf },
	};

	if (i2c_transfer(dvo->i2c, msgs, 2) == 2) {
		*val = in_buf[0];
		return 1;
	}
	if (!dvo->quiet)
		i915_dbg("[drm] i915: DVO: unable to read register 0x%02x from %s:%02x\n",
			 reg, dvo->i2c->name, dvo->desc->addr);
	return 0;
}

static int dvo_writeb(struct dvo_dev *dvo, uint8_t reg, uint8_t val)
{
	uint8_t out_buf[2] = { reg, val };
	struct i2c_msg msg = {
		.addr = dvo->desc->addr, .flags = 0, .len = 2, .buf = out_buf,
	};

	if (i2c_transfer(dvo->i2c, &msg, 1) == 1)
		return 1;
	if (!dvo->quiet)
		i915_dbg("[drm] i915: DVO: unable to write register 0x%02x to %s:%02x\n",
			 reg, dvo->i2c->name, dvo->desc->addr);
	return 0;
}

/* ---- Chrontel CH7xxx DVI transmitters --------------------------------------------- */

#define CH7xxx_REG_VID 0x4a
#define CH7xxx_REG_DID 0x4b

#define CH7011_VID 0x83 /* 7010 as well */
#define CH7010B_VID 0x05
#define CH7009A_VID 0x84
#define CH7009B_VID 0x85
#define CH7301_VID 0x95

#define CH7xxx_DID 0x17
#define CH7010_DID 0x16

#define CH7xxx_NUM_REGS 0x4c

#define CH7xxx_IDF 0x1f
#define CH7xxx_IDF_IBS (1 << 7)
#define CH7xxx_IDF_HSP (1 << 3)
#define CH7xxx_IDF_VSP (1 << 4)

#define CH7xxx_CONNECTION_DETECT 0x20
#define CH7xxx_CDET_DVI (1 << 5)

#define CH7xxx_DAC_CNTL 0x21
#define CH7xxx_SYNCO_VGA_HSYNC (1 << 3)

#define CH7xxx_CLOCK_OUTPUT 0x22
#define CH7xxx_BCOEN (1 << 4)
#define CH7xxx_BCO_VGA_VSYNC (6 << 0)

#define CH7xxx_TCTL 0x31
#define CH7xxx_TVCO 0x32
#define CH7xxx_TPCP 0x33
#define CH7xxx_TPD 0x34
#define CH7xxx_TPVT 0x35
#define CH7xxx_TLPF 0x36
#define CH7xxx_TCT 0x37

#define CH7xxx_PM 0x49
#define CH7xxx_PM_FPD (1 << 0)
#define CH7xxx_PM_DVIL (1 << 6)
#define CH7xxx_PM_DVIP (1 << 7)

static const struct {
	uint8_t vid;
	const char *name;
} ch7xxx_ids[] = {
	{ CH7011_VID, "CH7011" },
	{ CH7010B_VID, "CH7010B" },
	{ CH7009A_VID, "CH7009A" },
	{ CH7009B_VID, "CH7009B" },
	{ CH7301_VID, "CH7301" },
};

static const struct {
	uint8_t did;
	const char *name;
} ch7xxx_dids[] = {
	{ CH7xxx_DID, "CH7XXX" },
	{ CH7010_DID, "CH7010B" },
};

static int ch7xxx_init(struct dvo_dev *dvo)
{
	const char *name = NULL, *devid = NULL;
	uint8_t vendor, device;
	int i;

	dvo->quiet = 1;
	if (!dvo_readb(dvo, CH7xxx_REG_VID, &vendor))
		return 0;
	for (i = 0; i < DVO_ARRAY_SIZE(ch7xxx_ids); i++)
		if (ch7xxx_ids[i].vid == vendor)
			name = ch7xxx_ids[i].name;
	if (!name) {
		i915_dbg("[drm] i915: DVO: ch7xxx not detected; got VID 0x%02x from %s target %d\n",
			 vendor, dvo->i2c->name, dvo->desc->addr);
		return 0;
	}

	if (!dvo_readb(dvo, CH7xxx_REG_DID, &device))
		return 0;
	for (i = 0; i < DVO_ARRAY_SIZE(ch7xxx_dids); i++)
		if (ch7xxx_dids[i].did == device)
			devid = ch7xxx_dids[i].name;
	if (!devid) {
		i915_dbg("[drm] i915: DVO: ch7xxx not detected; got DID 0x%02x from %s target %d\n",
			 device, dvo->i2c->name, dvo->desc->addr);
		return 0;
	}

	dvo->quiet = 0;
	i915_dbg("[drm] i915: DVO: detected %s chipset, vendor/device ID 0x%02x/0x%02x\n",
		 name, vendor, device);
	return 1;
}

static int ch7xxx_detect(struct dvo_dev *dvo)
{
	uint8_t cdet = 0, orig_pm = 0, pm;

	/* The connection detect only works with the DVI link powered. */
	dvo_readb(dvo, CH7xxx_PM, &orig_pm);
	pm = orig_pm;
	pm &= ~CH7xxx_PM_FPD;
	pm |= CH7xxx_PM_DVIL | CH7xxx_PM_DVIP;
	dvo_writeb(dvo, CH7xxx_PM, pm);

	dvo_readb(dvo, CH7xxx_CONNECTION_DETECT, &cdet);

	dvo_writeb(dvo, CH7xxx_PM, orig_pm);

	return (cdet & CH7xxx_CDET_DVI) ? 1 : 0;
}

static int ch7xxx_mode_valid(struct dvo_dev *dvo, const struct drm_mode_modeinfo *mode)
{
	(void)dvo;
	if (mode->clock > 165000)
		return DVO_MODE_CLOCK_HIGH;
	return DVO_MODE_OK;
}

static void ch7xxx_mode_set(struct dvo_dev *dvo, const struct drm_mode_modeinfo *mode,
			    const struct drm_mode_modeinfo *adj)
{
	uint8_t tvco, tpcp, tpd, tlpf, idf = 0;

	(void)adj;
	/* The transmitter PLL's loop settings for low and high clocks. */
	if (mode->clock <= 65000) {
		tvco = 0x23;
		tpcp = 0x08;
		tpd = 0x16;
		tlpf = 0x60;
	} else {
		tvco = 0x2d;
		tpcp = 0x06;
		tpd = 0x26;
		tlpf = 0xa0;
	}

	dvo_writeb(dvo, CH7xxx_TCTL, 0x00);
	dvo_writeb(dvo, CH7xxx_TVCO, tvco);
	dvo_writeb(dvo, CH7xxx_TPCP, tpcp);
	dvo_writeb(dvo, CH7xxx_TPD, tpd);
	dvo_writeb(dvo, CH7xxx_TPVT, 0x30);
	dvo_writeb(dvo, CH7xxx_TLPF, tlpf);
	dvo_writeb(dvo, CH7xxx_TCT, 0x00);

	dvo_readb(dvo, CH7xxx_IDF, &idf);
	idf |= CH7xxx_IDF_IBS;
	idf &= ~(CH7xxx_IDF_HSP | CH7xxx_IDF_VSP);
	if (mode->flags & DRM_MODE_FLAG_PHSYNC)
		idf |= CH7xxx_IDF_HSP;
	if (mode->flags & DRM_MODE_FLAG_PVSYNC)
		idf |= CH7xxx_IDF_VSP;
	dvo_writeb(dvo, CH7xxx_IDF, idf);

	dvo_writeb(dvo, CH7xxx_DAC_CNTL, CH7xxx_SYNCO_VGA_HSYNC);
	dvo_writeb(dvo, CH7xxx_CLOCK_OUTPUT, CH7xxx_BCOEN | CH7xxx_BCO_VGA_VSYNC);
}

static void ch7xxx_dpms(struct dvo_dev *dvo, int enable)
{
	if (enable)
		dvo_writeb(dvo, CH7xxx_PM, CH7xxx_PM_DVIL | CH7xxx_PM_DVIP);
	else
		dvo_writeb(dvo, CH7xxx_PM, CH7xxx_PM_FPD);
}

static int ch7xxx_get_hw_state(struct dvo_dev *dvo)
{
	uint8_t val = 0;

	dvo_readb(dvo, CH7xxx_PM, &val);
	return (val & (CH7xxx_PM_DVIL | CH7xxx_PM_DVIP)) ? 1 : 0;
}

static void ch7xxx_dump_regs(struct dvo_dev *dvo)
{
	int i;

	for (i = 0; i < CH7xxx_NUM_REGS; i++) {
		uint8_t val = 0;

		dvo_readb(dvo, (uint8_t)i, &val);
		i915_dbg("[drm] i915: DVO: ch7xxx %02X: %02X\n", i, val);
	}
}

static const struct dvo_chip_ops ch7xxx_ops = {
	.init = ch7xxx_init,
	.detect = ch7xxx_detect,
	.mode_valid = ch7xxx_mode_valid,
	.mode_set = ch7xxx_mode_set,
	.dpms = ch7xxx_dpms,
	.get_hw_state = ch7xxx_get_hw_state,
	.dump_regs = ch7xxx_dump_regs,
};

/* ---- Silicon Image SiI164 DVI transmitter ----------------------------------------- */

#define SIL164_VID 0x0001
#define SIL164_DID 0x0006

#define SIL164_VID_LO 0x00
#define SIL164_DID_LO 0x02
#define SIL164_FREQ_LO 0x06
#define SIL164_FREQ_HI 0x07

#define SIL164_REG8 0x08
#define SIL164_8_VEN (1 << 5)
#define SIL164_8_HEN (1 << 4)
#define SIL164_8_PD (1 << 0)

#define SIL164_REG9 0x09
#define SIL164_9_TSEL (1 << 3)
#define SIL164_9_HTPLG (1 << 1)

#define SIL164_REGC 0x0c
#define SIL164_C_PLLF_REC (4 << 1)
#define SIL164_C_PFEN (1 << 0)

static int sil164_init(struct dvo_dev *dvo)
{
	uint8_t ch;

	dvo->quiet = 1;
	if (!dvo_readb(dvo, SIL164_VID_LO, &ch))
		return 0;
	if (ch != (SIL164_VID & 0xff)) {
		i915_dbg("[drm] i915: DVO: sil164 not detected, got %d from %s target %d\n",
			 ch, dvo->i2c->name, dvo->desc->addr);
		return 0;
	}
	if (!dvo_readb(dvo, SIL164_DID_LO, &ch))
		return 0;
	if (ch != (SIL164_DID & 0xff)) {
		i915_dbg("[drm] i915: DVO: sil164 not detected, got %d from %s target %d\n",
			 ch, dvo->i2c->name, dvo->desc->addr);
		return 0;
	}
	dvo->quiet = 0;
	i915_dbg("[drm] i915: DVO: sil164 controller found\n");
	return 1;
}

static int sil164_detect(struct dvo_dev *dvo)
{
	uint8_t reg9 = 0;

	dvo_readb(dvo, SIL164_REG9, &reg9);
	return (reg9 & SIL164_9_HTPLG) ? 1 : 0;
}

static int sil164_mode_valid(struct dvo_dev *dvo, const struct drm_mode_modeinfo *mode)
{
	(void)dvo;
	(void)mode;
	return DVO_MODE_OK;
}

static void sil164_mode_set(struct dvo_dev *dvo, const struct drm_mode_modeinfo *mode,
			    const struct drm_mode_modeinfo *adj)
{
	(void)mode;
	(void)adj;
	/* The chip has no clock dependencies in its mode setup: sync and
	 * data enables on, the internal source termination select, and the
	 * recommended PLL filter setting. */
	dvo_writeb(dvo, SIL164_REG8, SIL164_8_VEN | SIL164_8_HEN);
	dvo_writeb(dvo, SIL164_REG9, SIL164_9_TSEL);
	dvo_writeb(dvo, SIL164_REGC, SIL164_C_PLLF_REC | SIL164_C_PFEN);
}

static void sil164_dpms(struct dvo_dev *dvo, int enable)
{
	uint8_t ch;

	if (!dvo_readb(dvo, SIL164_REG8, &ch))
		return;
	/* PD is active low: set means powered up. */
	if (enable)
		ch |= SIL164_8_PD;
	else
		ch &= ~SIL164_8_PD;
	dvo_writeb(dvo, SIL164_REG8, ch);
}

static int sil164_get_hw_state(struct dvo_dev *dvo)
{
	uint8_t ch;

	if (!dvo_readb(dvo, SIL164_REG8, &ch))
		return 0;
	return (ch & SIL164_8_PD) ? 1 : 0;
}

static void sil164_dump_regs(struct dvo_dev *dvo)
{
	uint8_t val = 0;

	dvo_readb(dvo, SIL164_FREQ_LO, &val);
	i915_dbg("[drm] i915: DVO: SIL164_FREQ_LO: 0x%02x\n", val);
	dvo_readb(dvo, SIL164_FREQ_HI, &val);
	i915_dbg("[drm] i915: DVO: SIL164_FREQ_HI: 0x%02x\n", val);
	dvo_readb(dvo, SIL164_REG8, &val);
	i915_dbg("[drm] i915: DVO: SIL164_REG8: 0x%02x\n", val);
	dvo_readb(dvo, SIL164_REG9, &val);
	i915_dbg("[drm] i915: DVO: SIL164_REG9: 0x%02x\n", val);
	dvo_readb(dvo, SIL164_REGC, &val);
	i915_dbg("[drm] i915: DVO: SIL164_REGC: 0x%02x\n", val);
}

static const struct dvo_chip_ops sil164_ops = {
	.init = sil164_init,
	.detect = sil164_detect,
	.mode_valid = sil164_mode_valid,
	.mode_set = sil164_mode_set,
	.dpms = sil164_dpms,
	.get_hw_state = sil164_get_hw_state,
	.dump_regs = sil164_dump_regs,
};

/* ---- TI TFP410 DVI transmitter ------------------------------------------------------ */

#define TFP410_VID 0x014C
#define TFP410_DID 0x0410

#define TFP410_VID_LO 0x00
#define TFP410_DID_LO 0x02
#define TFP410_REV 0x04

#define TFP410_CTL_1 0x08
#define TFP410_CTL_1_PD (1 << 0)

#define TFP410_CTL_2 0x09
#define TFP410_CTL_2_RSEN (1 << 2)

#define TFP410_CTL_3 0x0A
#define TFP410_USERCFG 0x0B
#define TFP410_DE_DLY 0x32
#define TFP410_DE_CTL 0x33
#define TFP410_DE_TOP 0x34
#define TFP410_DE_CNT_LO 0x36
#define TFP410_DE_CNT_HI 0x37
#define TFP410_DE_LIN_LO 0x38
#define TFP410_DE_LIN_HI 0x39
#define TFP410_H_RES_LO 0x3A
#define TFP410_H_RES_HI 0x3B
#define TFP410_V_RES_LO 0x3C
#define TFP410_V_RES_HI 0x3D

static int tfp410_getid(struct dvo_dev *dvo, uint8_t addr)
{
	uint8_t ch1, ch2;

	if (dvo_readb(dvo, addr, &ch1) && dvo_readb(dvo, addr + 1, &ch2))
		return ((ch2 << 8) & 0xff00) | (ch1 & 0x00ff);
	return -1;
}

static int tfp410_init(struct dvo_dev *dvo)
{
	int id;

	dvo->quiet = 1;
	id = tfp410_getid(dvo, TFP410_VID_LO);
	if (id != TFP410_VID) {
		i915_dbg("[drm] i915: DVO: tfp410 not detected, got VID %X from %s target %d\n",
			 id, dvo->i2c->name, dvo->desc->addr);
		return 0;
	}
	id = tfp410_getid(dvo, TFP410_DID_LO);
	if (id != TFP410_DID) {
		i915_dbg("[drm] i915: DVO: tfp410 not detected, got DID %X from %s target %d\n",
			 id, dvo->i2c->name, dvo->desc->addr);
		return 0;
	}
	dvo->quiet = 0;
	return 1;
}

static int tfp410_detect(struct dvo_dev *dvo)
{
	uint8_t ctl2;

	/* Receiver sense: the monitor's termination is seen on the link. */
	if (dvo_readb(dvo, TFP410_CTL_2, &ctl2))
		return (ctl2 & TFP410_CTL_2_RSEN) ? 1 : 0;
	return 0;
}

static int tfp410_mode_valid(struct dvo_dev *dvo, const struct drm_mode_modeinfo *mode)
{
	(void)dvo;
	(void)mode;
	return DVO_MODE_OK;
}

static void tfp410_mode_set(struct dvo_dev *dvo, const struct drm_mode_modeinfo *mode,
			    const struct drm_mode_modeinfo *adj)
{
	/* No clock dependencies: the power-on defaults do. */
	(void)dvo;
	(void)mode;
	(void)adj;
}

static void tfp410_dpms(struct dvo_dev *dvo, int enable)
{
	uint8_t ctl1;

	if (!dvo_readb(dvo, TFP410_CTL_1, &ctl1))
		return;
	if (enable)
		ctl1 |= TFP410_CTL_1_PD;
	else
		ctl1 &= ~TFP410_CTL_1_PD;
	dvo_writeb(dvo, TFP410_CTL_1, ctl1);
}

static int tfp410_get_hw_state(struct dvo_dev *dvo)
{
	uint8_t ctl1;

	if (!dvo_readb(dvo, TFP410_CTL_1, &ctl1))
		return 0;
	return (ctl1 & TFP410_CTL_1_PD) ? 1 : 0;
}

static void tfp410_dump_regs(struct dvo_dev *dvo)
{
	uint8_t val = 0, val2 = 0;

	dvo_readb(dvo, TFP410_REV, &val);
	i915_dbg("[drm] i915: DVO: TFP410_REV: 0x%02X\n", val);
	dvo_readb(dvo, TFP410_CTL_1, &val);
	i915_dbg("[drm] i915: DVO: TFP410_CTL1: 0x%02X\n", val);
	dvo_readb(dvo, TFP410_CTL_2, &val);
	i915_dbg("[drm] i915: DVO: TFP410_CTL2: 0x%02X\n", val);
	dvo_readb(dvo, TFP410_CTL_3, &val);
	i915_dbg("[drm] i915: DVO: TFP410_CTL3: 0x%02X\n", val);
	dvo_readb(dvo, TFP410_USERCFG, &val);
	i915_dbg("[drm] i915: DVO: TFP410_USERCFG: 0x%02X\n", val);
	dvo_readb(dvo, TFP410_DE_DLY, &val);
	i915_dbg("[drm] i915: DVO: TFP410_DE_DLY: 0x%02X\n", val);
	dvo_readb(dvo, TFP410_DE_CTL, &val);
	i915_dbg("[drm] i915: DVO: TFP410_DE_CTL: 0x%02X\n", val);
	dvo_readb(dvo, TFP410_DE_TOP, &val);
	i915_dbg("[drm] i915: DVO: TFP410_DE_TOP: 0x%02X\n", val);
	dvo_readb(dvo, TFP410_DE_CNT_LO, &val);
	dvo_readb(dvo, TFP410_DE_CNT_HI, &val2);
	i915_dbg("[drm] i915: DVO: TFP410_DE_CNT: 0x%02X%02X\n", val2, val);
	dvo_readb(dvo, TFP410_DE_LIN_LO, &val);
	dvo_readb(dvo, TFP410_DE_LIN_HI, &val2);
	i915_dbg("[drm] i915: DVO: TFP410_DE_LIN: 0x%02X%02X\n", val2, val);
	dvo_readb(dvo, TFP410_H_RES_LO, &val);
	dvo_readb(dvo, TFP410_H_RES_HI, &val2);
	i915_dbg("[drm] i915: DVO: TFP410_H_RES: 0x%02X%02X\n", val2, val);
	dvo_readb(dvo, TFP410_V_RES_LO, &val);
	dvo_readb(dvo, TFP410_V_RES_HI, &val2);
	i915_dbg("[drm] i915: DVO: TFP410_V_RES: 0x%02X%02X\n", val2, val);
}

static const struct dvo_chip_ops tfp410_ops = {
	.init = tfp410_init,
	.detect = tfp410_detect,
	.mode_valid = tfp410_mode_valid,
	.mode_set = tfp410_mode_set,
	.dpms = tfp410_dpms,
	.get_hw_state = tfp410_get_hw_state,
	.dump_regs = tfp410_dump_regs,
};

/* ---- the GPIO pair, bit-banged (the IVCH's register protocol) ----------------------- */

/* The IVCH reads a register in one odd transaction: START, its address
 * with the read bit, then the register index clocked out by the master,
 * then the two data bytes clocked in, STOP.  A plain write-then-read with
 * a repeated START is not what it understands, and the GMBUS controller
 * cannot do this, so the pair is driven by hand here at about 50 kHz with
 * clock stretching honoured. */

#define DVO_BB_UDELAY 10 /* rise/fall time, us */
#define DVO_BB_TIMEOUT_US 2200 /* a slave holding the clock low */

static uint32_t dvo_bb_reserved(struct dvo_dev *dvo)
{
	struct lg_display *d = dvo->d;

	/* The pull-up disable bits must be kept, except on the i830/i845G
	 * which have none. */
	if (d->is_i830 || d->is_i845)
		return 0;
	return lg_rd(d, dvo->gpio_reg) & (GPIO_DATA_PULLUP_DISABLE | GPIO_CLOCK_PULLUP_DISABLE);
}

static int dvo_bb_get_scl(struct dvo_dev *dvo)
{
	uint32_t r = dvo_bb_reserved(dvo);

	lg_wr(dvo->d, dvo->gpio_reg, r | GPIO_CLOCK_DIR_MASK);
	lg_wr(dvo->d, dvo->gpio_reg, r);
	return (lg_rd(dvo->d, dvo->gpio_reg) & GPIO_CLOCK_VAL_IN) != 0;
}

static int dvo_bb_get_sda(struct dvo_dev *dvo)
{
	uint32_t r = dvo_bb_reserved(dvo);

	lg_wr(dvo->d, dvo->gpio_reg, r | GPIO_DATA_DIR_MASK);
	lg_wr(dvo->d, dvo->gpio_reg, r);
	return (lg_rd(dvo->d, dvo->gpio_reg) & GPIO_DATA_VAL_IN) != 0;
}

/* High is "released" (input, the pull-up wins); low is driven. */
static void dvo_bb_set_scl(struct dvo_dev *dvo, int high)
{
	uint32_t bits;

	if (high)
		bits = GPIO_CLOCK_DIR_IN | GPIO_CLOCK_DIR_MASK;
	else
		bits = GPIO_CLOCK_DIR_OUT | GPIO_CLOCK_DIR_MASK | GPIO_CLOCK_VAL_MASK;
	lg_wr(dvo->d, dvo->gpio_reg, dvo_bb_reserved(dvo) | bits);
	lg_posting_read(dvo->d, dvo->gpio_reg);
}

static void dvo_bb_set_sda(struct dvo_dev *dvo, int high)
{
	uint32_t bits;

	if (high)
		bits = GPIO_DATA_DIR_IN | GPIO_DATA_DIR_MASK;
	else
		bits = GPIO_DATA_DIR_OUT | GPIO_DATA_DIR_MASK | GPIO_DATA_VAL_MASK;
	lg_wr(dvo->d, dvo->gpio_reg, dvo_bb_reserved(dvo) | bits);
	lg_posting_read(dvo->d, dvo->gpio_reg);
}

static void dvo_bb_sdalo(struct dvo_dev *dvo)
{
	dvo_bb_set_sda(dvo, 0);
	lg_udelay((DVO_BB_UDELAY + 1) / 2);
}

static void dvo_bb_sdahi(struct dvo_dev *dvo)
{
	dvo_bb_set_sda(dvo, 1);
	lg_udelay((DVO_BB_UDELAY + 1) / 2);
}

static void dvo_bb_scllo(struct dvo_dev *dvo)
{
	dvo_bb_set_scl(dvo, 0);
	lg_udelay(DVO_BB_UDELAY / 2);
}

/* Release the clock and wait until it is really high (a slave may
 * stretch it). */
static int dvo_bb_sclhi(struct dvo_dev *dvo)
{
	uint32_t waited = 0;

	dvo_bb_set_scl(dvo, 1);
	while (!dvo_bb_get_scl(dvo)) {
		if (waited >= DVO_BB_TIMEOUT_US) {
			if (dvo_bb_get_scl(dvo))
				break;
			return -ETIMEDOUT;
		}
		lg_udelay(1);
		waited++;
	}
	lg_udelay(DVO_BB_UDELAY);
	return 0;
}

static void dvo_bb_start(struct dvo_dev *dvo)
{
	/* SCL and SDA are high: SDA falls first. */
	dvo_bb_set_sda(dvo, 0);
	lg_udelay(DVO_BB_UDELAY);
	dvo_bb_scllo(dvo);
}

static void dvo_bb_stop(struct dvo_dev *dvo)
{
	dvo_bb_sdalo(dvo);
	dvo_bb_sclhi(dvo);
	dvo_bb_set_sda(dvo, 1);
	lg_udelay(DVO_BB_UDELAY);
}

/* One byte out, MSB first; 1 when the slave acknowledged, 0 when not,
 * negative on a clock timeout. */
static int dvo_bb_outb(struct dvo_dev *dvo, uint8_t c)
{
	int i, ack;

	for (i = 7; i >= 0; i--) {
		dvo_bb_set_sda(dvo, (c >> i) & 1);
		lg_udelay((DVO_BB_UDELAY + 1) / 2);
		if (dvo_bb_sclhi(dvo) < 0)
			return -ETIMEDOUT;
		dvo_bb_scllo(dvo);
	}
	dvo_bb_sdahi(dvo);
	if (dvo_bb_sclhi(dvo) < 0)
		return -ETIMEDOUT;
	ack = !dvo_bb_get_sda(dvo);
	dvo_bb_scllo(dvo);
	return ack;
}

static int dvo_bb_inb(struct dvo_dev *dvo)
{
	int i;
	uint8_t indata = 0;

	dvo_bb_sdahi(dvo);
	for (i = 0; i < 8; i++) {
		if (dvo_bb_sclhi(dvo) < 0)
			return -ETIMEDOUT;
		indata <<= 1;
		if (dvo_bb_get_sda(dvo))
			indata |= 1;
		dvo_bb_set_scl(dvo, 0);
		lg_udelay(i == 7 ? DVO_BB_UDELAY / 2 : DVO_BB_UDELAY);
	}
	return indata;
}

static int dvo_bb_acknak(struct dvo_dev *dvo, int is_ack)
{
	if (is_ack)
		dvo_bb_set_sda(dvo, 0);
	lg_udelay((DVO_BB_UDELAY + 1) / 2);
	if (dvo_bb_sclhi(dvo) < 0)
		return -ETIMEDOUT;
	dvo_bb_scllo(dvo);
	return 0;
}

/* Address the slave, with one retry after a NAK. */
static int dvo_bb_address(struct dvo_dev *dvo, uint8_t addr8)
{
	int i, ret = 0;

	for (i = 0; i <= 1; i++) {
		ret = dvo_bb_outb(dvo, addr8);
		if (ret == 1 || i == 1)
			break;
		dvo_bb_stop(dvo);
		lg_udelay(DVO_BB_UDELAY);
		dvo_bb_start(dvo);
	}
	return ret == 1;
}

static void dvo_bb_pre_xfer(struct dvo_dev *dvo)
{
	struct lg_display *d = dvo->d;

	/* The controller off the pins while they are bit-banged. */
	lg_wr(d, GMBUS0_REG(d->gmbus_base), 0);
	lg_wr(d, GMBUS4_REG(d->gmbus_base), 0);
	dvo_bb_set_sda(dvo, 1);
	dvo_bb_set_scl(dvo, 1);
	lg_udelay(DVO_BB_UDELAY);
}

static void dvo_bb_post_xfer(struct dvo_dev *dvo)
{
	dvo_bb_set_sda(dvo, 1);
	dvo_bb_set_scl(dvo, 1);
}

/* ---- Intel 82807AA (IVCH) LVDS controller --------------------------------------------- */

/* VCH revision and its own bus address */
#define IVCH_VR00 0x00
#define IVCH_VR00_BASE_ADDRESS_MASK 0x007f

/* Functionality enable */
#define IVCH_VR01 0x01
#define IVCH_VR01_PANEL_FIT_ENABLE (1 << 3)
/* The LCD output; never together with the DVO repeater bypass. */
#define IVCH_VR01_LCD_ENABLE (1 << 2)
#define IVCH_VR01_DVO_BYPASS_ENABLE (1 << 1)
#define IVCH_VR01_DVO_ENABLE (1 << 0)
/* Dithering for 18 bpp panels (undocumented). */
#define IVCH_VR01_DITHER_ENABLE (1 << 4)

/* LCD interface format */
#define IVCH_VR10 0x10
#define IVCH_VR10_INTERFACE_1X18 (0 << 2)
#define IVCH_VR10_INTERFACE_1X24 (1 << 2)
#define IVCH_VR10_INTERFACE_2X18 (2 << 2)
#define IVCH_VR10_INTERFACE_2X24 (3 << 2)
#define IVCH_VR10_INTERFACE_DEPTH_MASK (3 << 2)

/* panel size */
#define IVCH_VR20 0x20
#define IVCH_VR21 0x21

/* panel power status: not yet in a safe power-off state */
#define IVCH_VR30 0x30
#define IVCH_VR30_PANEL_ON (1 << 15)

#define IVCH_VR40 0x40
#define IVCH_VR40_STALL_ENABLE (1 << 13)
#define IVCH_VR40_VERTICAL_INTERP_ENABLE (1 << 12)
#define IVCH_VR40_ENHANCED_PANEL_FITTING (1 << 11)
#define IVCH_VR40_HORIZONTAL_INTERP_ENABLE (1 << 10)
#define IVCH_VR40_AUTO_RATIO_ENABLE (1 << 9)
#define IVCH_VR40_CLOCK_GATING_ENABLE (1 << 8)

/* Panel fitting ratios: (((image - 1) << 16) / (panel - 1)) >> 2 */
#define IVCH_VR41 0x41 /* vertical */
#define IVCH_VR42 0x42 /* horizontal */
#define IVCH_VR43 0x43

/* GPIO 0 (the backlight) to 8 */
#define IVCH_VR80 0x80
#define IVCH_VR88 0x88

/* graphics BIOS scratch registers: panel type, status */
#define IVCH_VR8E 0x8e
#define IVCH_VR8F 0x8f

static void ivch_dump_regs(struct dvo_dev *dvo);

/* Each of the 256 registers is 16 bits wide, low byte first. */
static int ivch_read(struct dvo_dev *dvo, uint8_t addr, uint16_t *data)
{
	int lo, hi, ok = 0;

	dvo_bb_pre_xfer(dvo);
	dvo_bb_start(dvo);
	if (!dvo_bb_address(dvo, (uint8_t)((dvo->desc->addr << 1) | 1)))
		goto out;
	if (dvo_bb_outb(dvo, addr) != 1)
		goto out;
	lo = dvo_bb_inb(dvo);
	if (lo < 0 || dvo_bb_acknak(dvo, 1) < 0)
		goto out;
	hi = dvo_bb_inb(dvo);
	if (hi < 0 || dvo_bb_acknak(dvo, 0) < 0)
		goto out;
	*data = (uint16_t)((hi << 8) | lo);
	ok = 1;
out:
	dvo_bb_stop(dvo);
	dvo_bb_post_xfer(dvo);
	if (!ok && !dvo->quiet)
		i915_dbg("[drm] i915: DVO: unable to read register 0x%02x from ivch:%02x\n",
			 addr, dvo->desc->addr);
	return ok;
}

static int ivch_write(struct dvo_dev *dvo, uint8_t addr, uint16_t data)
{
	uint8_t buf[3] = { addr, (uint8_t)(data & 0xff), (uint8_t)(data >> 8) };
	int i, ok = 0;

	dvo_bb_pre_xfer(dvo);
	dvo_bb_start(dvo);
	if (!dvo_bb_address(dvo, (uint8_t)(dvo->desc->addr << 1)))
		goto out;
	for (i = 0; i < 3; i++)
		if (dvo_bb_outb(dvo, buf[i]) != 1)
			goto out;
	ok = 1;
out:
	dvo_bb_stop(dvo);
	dvo_bb_post_xfer(dvo);
	if (!ok && !dvo->quiet)
		i915_dbg("[drm] i915: DVO: unable to write register 0x%02x to ivch:%02x\n",
			 addr, dvo->desc->addr);
	return ok;
}

static int ivch_init(struct dvo_dev *dvo)
{
	uint16_t temp;
	int i;

	dvo->quiet = 1;
	if (!ivch_read(dvo, IVCH_VR00, &temp))
		return 0;
	dvo->quiet = 0;

	/* The identification bits are probably zero, which is not very
	 * unique; the base address field must match the address the chip
	 * answered on. */
	if ((temp & IVCH_VR00_BASE_ADDRESS_MASK) != dvo->desc->addr) {
		i915_dbg("[drm] i915: DVO: ivch detect failed due to address mismatch (%d vs %d)\n",
			 temp & IVCH_VR00_BASE_ADDRESS_MASK, dvo->desc->addr);
		return 0;
	}

	ivch_read(dvo, IVCH_VR20, &dvo->ivch_width);
	ivch_read(dvo, IVCH_VR21, &dvo->ivch_height);

	/* Keep what the firmware set up, to write it back after a resume. */
	for (i = 0; i < IVCH_NUM_BACKUP; i++)
		ivch_read(dvo, (uint8_t)ivch_backup_addresses[i], &dvo->ivch_backup[i]);

	if (I915_DEBUG)
		ivch_dump_regs(dvo);
	return 1;
}

static int ivch_detect(struct dvo_dev *dvo)
{
	(void)dvo;
	return 1;
}

static int ivch_mode_valid(struct dvo_dev *dvo, const struct drm_mode_modeinfo *mode)
{
	(void)dvo;
	if (mode->clock > 112000)
		return DVO_MODE_CLOCK_HIGH;
	return DVO_MODE_OK;
}

/* Some firmware does not restore the chip over a suspend: write back the
 * registers saved at probe time. */
static void ivch_reset(struct dvo_dev *dvo)
{
	int i;

	i915_dbg("[drm] i915: DVO: resetting the IVCH registers\n");
	ivch_write(dvo, IVCH_VR10, 0x0000);
	for (i = 0; i < IVCH_NUM_BACKUP; i++)
		ivch_write(dvo, (uint8_t)ivch_backup_addresses[i], dvo->ivch_backup[i]);
}

static void ivch_dpms(struct dvo_dev *dvo, int enable)
{
	uint16_t vr01, vr30;
	int i;

	ivch_reset(dvo);

	if (!ivch_read(dvo, IVCH_VR01, &vr01))
		return;

	/* GPIO 0 drives the backlight. */
	ivch_write(dvo, IVCH_VR80, enable ? 1 : 0);

	if (enable)
		vr01 |= IVCH_VR01_LCD_ENABLE | IVCH_VR01_DVO_ENABLE;
	else
		vr01 &= ~(IVCH_VR01_LCD_ENABLE | IVCH_VR01_DVO_ENABLE);
	ivch_write(dvo, IVCH_VR01, vr01);

	/* Wait for the panel to make its power transition. */
	for (i = 0; i < 100; i++) {
		if (!ivch_read(dvo, IVCH_VR30, &vr30))
			break;
		if (((vr30 & IVCH_VR30_PANEL_ON) != 0) == (enable != 0))
			break;
		lg_udelay(1000);
	}
	/* And some more: the VCH may fail to resync without it. */
	lg_mdelay(16);
}

static int ivch_get_hw_state(struct dvo_dev *dvo)
{
	uint16_t vr01;

	ivch_reset(dvo);
	if (!ivch_read(dvo, IVCH_VR01, &vr01))
		return 0;
	return (vr01 & IVCH_VR01_LCD_ENABLE) ? 1 : 0;
}

static void ivch_mode_set(struct dvo_dev *dvo, const struct drm_mode_modeinfo *mode,
			  const struct drm_mode_modeinfo *adj)
{
	uint16_t vr40, vr01 = 0, vr10;

	ivch_reset(dvo);

	vr10 = dvo->ivch_backup[IVCH_NUM_BACKUP - 1];

	/* Dither for 18 bpp panel interfaces. */
	vr10 &= IVCH_VR10_INTERFACE_DEPTH_MASK;
	if (vr10 == IVCH_VR10_INTERFACE_2X18 || vr10 == IVCH_VR10_INTERFACE_1X18)
		vr01 = IVCH_VR01_DITHER_ENABLE;

	vr40 = IVCH_VR40_STALL_ENABLE | IVCH_VR40_VERTICAL_INTERP_ENABLE |
	       IVCH_VR40_HORIZONTAL_INTERP_ENABLE;

	if (mode->hdisplay != adj->hdisplay || mode->vdisplay != adj->vdisplay) {
		uint16_t x_ratio, y_ratio;

		/* The chip's own fitter scales the picture onto the panel. */
		vr01 |= IVCH_VR01_PANEL_FIT_ENABLE;
		vr40 |= IVCH_VR40_CLOCK_GATING_ENABLE;
		x_ratio = (uint16_t)((((uint32_t)(mode->hdisplay - 1) << 16) /
				      (uint32_t)(adj->hdisplay - 1)) >> 2);
		y_ratio = (uint16_t)((((uint32_t)(mode->vdisplay - 1) << 16) /
				      (uint32_t)(adj->vdisplay - 1)) >> 2);
		ivch_write(dvo, IVCH_VR42, x_ratio);
		ivch_write(dvo, IVCH_VR41, y_ratio);
	} else {
		vr01 &= ~IVCH_VR01_PANEL_FIT_ENABLE;
		vr40 &= ~IVCH_VR40_CLOCK_GATING_ENABLE;
	}
	vr40 &= ~IVCH_VR40_AUTO_RATIO_ENABLE;

	ivch_write(dvo, IVCH_VR01, vr01);
	ivch_write(dvo, IVCH_VR40, vr40);
}

static void ivch_dump_regs(struct dvo_dev *dvo)
{
	static const uint8_t regs[] = {
		IVCH_VR00, IVCH_VR01, IVCH_VR10, IVCH_VR30, IVCH_VR40,
		0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, IVCH_VR88,
		IVCH_VR8E, IVCH_VR8F,
	};
	int i;

	for (i = 0; i < DVO_ARRAY_SIZE(regs); i++) {
		uint16_t val = 0;

		ivch_read(dvo, regs[i], &val);
		i915_dbg("[drm] i915: DVO: ivch VR%02X: 0x%04x\n", regs[i], val);
	}
}

static const struct dvo_chip_ops ivch_ops = {
	.init = ivch_init,
	.dpms = ivch_dpms,
	.get_hw_state = ivch_get_hw_state,
	.mode_valid = ivch_mode_valid,
	.mode_set = ivch_mode_set,
	.detect = ivch_detect,
	.dump_regs = ivch_dump_regs,
};

/* ---- Chrontel CH7017/7018/7019 LVDS (and TV) encoder ------------------------------ */

#define CH7017_POWER_MANAGEMENT 0x49
#define CH7017_DAC0_POWER_DOWN (1 << 1)
#define CH7017_DAC1_POWER_DOWN (1 << 2)
#define CH7017_DAC2_POWER_DOWN (1 << 3)
#define CH7017_DAC3_POWER_DOWN (1 << 4)
#define CH7017_TV_POWER_DOWN_EN (1 << 5)

#define CH7017_DEVICE_ID 0x4b
#define CH7017_DEVICE_ID_VALUE 0x1b
#define CH7018_DEVICE_ID_VALUE 0x1a
#define CH7019_DEVICE_ID_VALUE 0x19

#define CH7017_HORIZONTAL_ACTIVE_PIXEL_INPUT 0x5f
#define CH7017_ACTIVE_INPUT_LINE_OUTPUT 0x60
#define CH7017_VERTICAL_ACTIVE_LINE_OUTPUT 0x61
#define CH7017_HORIZONTAL_ACTIVE_PIXEL_OUTPUT 0x62

#define CH7017_LVDS_POWER_DOWN 0x63
#define CH7017_LVDS_POWER_DOWN_EN (1 << 6)
#define CH7017_LVDS_POWER_DOWN_DEFAULT_RESERVED 0x08

#define CH7017_LVDS_PLL_FEEDBACK_DIV 0x71
#define CH7017_LVDS_PLL_FEED_BACK_DIVIDER_SHIFT 4
#define CH7017_LVDS_PLL_FEED_FORWARD_DIVIDER_SHIFT 0
#define CH7017_LVDS_PLL_FEEDBACK_DEFAULT_RESERVED 0x80

#define CH7017_LVDS_PLL_VCO_CONTROL 0x72
#define CH7017_LVDS_PLL_VCO_DEFAULT_RESERVED 0x80
#define CH7017_LVDS_PLL_VCO_SHIFT 4
#define CH7017_LVDS_PLL_POST_SCALE_DIV_SHIFT 0

#define CH7017_OUTPUTS_ENABLE 0x73
#define CH7017_CHARGE_PUMP_LOW 0x0
#define CH7017_CHARGE_PUMP_HIGH 0x3
#define CH7017_LVDS_CHANNEL_A (1 << 3)
#define CH7017_LVDS_CHANNEL_B (1 << 4)

#define CH7017_LVDS_CONTROL_2 0x78
#define CH7017_LOOP_FILTER_SHIFT 5
#define CH7017_PHASE_DETECTOR_SHIFT 0

static void ch7017_dump_regs(struct dvo_dev *dvo);
static void ch7017_dpms(struct dvo_dev *dvo, int enable);

static int ch7017_init(struct dvo_dev *dvo)
{
	const char *str;
	uint8_t val;

	dvo->quiet = 1;
	if (!dvo_readb(dvo, CH7017_DEVICE_ID, &val))
		return 0;

	switch (val) {
	case CH7017_DEVICE_ID_VALUE:
		str = "ch7017";
		break;
	case CH7018_DEVICE_ID_VALUE:
		str = "ch7018";
		break;
	case CH7019_DEVICE_ID_VALUE:
		str = "ch7019";
		break;
	default:
		i915_dbg("[drm] i915: DVO: ch701x not detected, got %d from %s target %d\n",
			 val, dvo->i2c->name, dvo->desc->addr);
		return 0;
	}
	dvo->quiet = 0;
	i915_dbg("[drm] i915: DVO: %s detected on %s, addr %d\n", str, dvo->i2c->name,
		 dvo->desc->addr);
	return 1;
}

static int ch7017_detect(struct dvo_dev *dvo)
{
	(void)dvo;
	return 1;
}

static int ch7017_mode_valid(struct dvo_dev *dvo, const struct drm_mode_modeinfo *mode)
{
	(void)dvo;
	if (mode->clock > 160000)
		return DVO_MODE_CLOCK_HIGH;
	return DVO_MODE_OK;
}

static void ch7017_mode_set(struct dvo_dev *dvo, const struct drm_mode_modeinfo *mode,
			    const struct drm_mode_modeinfo *adj)
{
	uint8_t lvds_pll_feedback_div, lvds_pll_vco_control;
	uint8_t outputs_enable, lvds_control_2, lvds_power_down;
	uint8_t horizontal_active_pixel_input;
	uint8_t horizontal_active_pixel_output, vertical_active_line_output;
	uint8_t active_input_line_output;

	(void)adj;
	if (I915_DEBUG) {
		i915_dbg("[drm] i915: DVO: ch7017 registers before mode setting\n");
		ch7017_dump_regs(dvo);
	}

	/* The LVDS PLL settings of the data sheet for the two clock ranges. */
	if (mode->clock < 100000) {
		outputs_enable = CH7017_LVDS_CHANNEL_A | CH7017_CHARGE_PUMP_LOW;
		lvds_pll_feedback_div = CH7017_LVDS_PLL_FEEDBACK_DEFAULT_RESERVED |
					(2 << CH7017_LVDS_PLL_FEED_BACK_DIVIDER_SHIFT) |
					(13 << CH7017_LVDS_PLL_FEED_FORWARD_DIVIDER_SHIFT);
		lvds_pll_vco_control = CH7017_LVDS_PLL_VCO_DEFAULT_RESERVED |
				       (2 << CH7017_LVDS_PLL_VCO_SHIFT) |
				       (3 << CH7017_LVDS_PLL_POST_SCALE_DIV_SHIFT);
		lvds_control_2 = (1 << CH7017_LOOP_FILTER_SHIFT) |
				 (0 << CH7017_PHASE_DETECTOR_SHIFT);
	} else {
		outputs_enable = CH7017_LVDS_CHANNEL_A | CH7017_CHARGE_PUMP_HIGH;
		lvds_pll_feedback_div = CH7017_LVDS_PLL_FEEDBACK_DEFAULT_RESERVED |
					(2 << CH7017_LVDS_PLL_FEED_BACK_DIVIDER_SHIFT) |
					(3 << CH7017_LVDS_PLL_FEED_FORWARD_DIVIDER_SHIFT);
		lvds_control_2 = (3 << CH7017_LOOP_FILTER_SHIFT) |
				 (0 << CH7017_PHASE_DETECTOR_SHIFT);
		/* There is no dual channel panel detection: above 100 MHz
		 * the panel is taken to be dual channel. */
		outputs_enable |= CH7017_LVDS_CHANNEL_B;
		lvds_pll_vco_control = CH7017_LVDS_PLL_VCO_DEFAULT_RESERVED |
				       (2 << CH7017_LVDS_PLL_VCO_SHIFT) |
				       (13 << CH7017_LVDS_PLL_POST_SCALE_DIV_SHIFT);
	}

	horizontal_active_pixel_input = mode->hdisplay & 0x00ff;
	vertical_active_line_output = mode->vdisplay & 0x00ff;
	horizontal_active_pixel_output = mode->hdisplay & 0x00ff;
	active_input_line_output = (uint8_t)(((mode->hdisplay & 0x0700) >> 8) |
					     (((mode->vdisplay & 0x0700) >> 8) << 3));
	lvds_power_down = (uint8_t)(CH7017_LVDS_POWER_DOWN_DEFAULT_RESERVED |
				    ((mode->hdisplay & 0x0700) >> 8));

	ch7017_dpms(dvo, 0);
	dvo_writeb(dvo, CH7017_HORIZONTAL_ACTIVE_PIXEL_INPUT, horizontal_active_pixel_input);
	dvo_writeb(dvo, CH7017_HORIZONTAL_ACTIVE_PIXEL_OUTPUT, horizontal_active_pixel_output);
	dvo_writeb(dvo, CH7017_VERTICAL_ACTIVE_LINE_OUTPUT, vertical_active_line_output);
	dvo_writeb(dvo, CH7017_ACTIVE_INPUT_LINE_OUTPUT, active_input_line_output);
	dvo_writeb(dvo, CH7017_LVDS_PLL_VCO_CONTROL, lvds_pll_vco_control);
	dvo_writeb(dvo, CH7017_LVDS_PLL_FEEDBACK_DIV, lvds_pll_feedback_div);
	dvo_writeb(dvo, CH7017_LVDS_CONTROL_2, lvds_control_2);
	dvo_writeb(dvo, CH7017_OUTPUTS_ENABLE, outputs_enable);

	/* The LVDS back on with the new settings. */
	dvo_writeb(dvo, CH7017_LVDS_POWER_DOWN, lvds_power_down);

	if (I915_DEBUG) {
		i915_dbg("[drm] i915: DVO: ch7017 registers after mode setting\n");
		ch7017_dump_regs(dvo);
	}
}

static void ch7017_dpms(struct dvo_dev *dvo, int enable)
{
	uint8_t val = 0;

	dvo_readb(dvo, CH7017_LVDS_POWER_DOWN, &val);

	/* TV and VGA off, and never on: only the LVDS side is used. */
	dvo_writeb(dvo, CH7017_POWER_MANAGEMENT,
		   CH7017_DAC0_POWER_DOWN | CH7017_DAC1_POWER_DOWN |
		   CH7017_DAC2_POWER_DOWN | CH7017_DAC3_POWER_DOWN |
		   CH7017_TV_POWER_DOWN_EN);

	if (enable)
		dvo_writeb(dvo, CH7017_LVDS_POWER_DOWN, val & ~CH7017_LVDS_POWER_DOWN_EN);
	else
		dvo_writeb(dvo, CH7017_LVDS_POWER_DOWN, val | CH7017_LVDS_POWER_DOWN_EN);

	/* There is no power status to poll: give the panel its time. */
	lg_mdelay(20);
}

static int ch7017_get_hw_state(struct dvo_dev *dvo)
{
	uint8_t val = 0;

	dvo_readb(dvo, CH7017_LVDS_POWER_DOWN, &val);
	return (val & CH7017_LVDS_POWER_DOWN_EN) ? 0 : 1;
}

static void ch7017_dump_regs(struct dvo_dev *dvo)
{
	static const uint8_t regs[] = {
		CH7017_HORIZONTAL_ACTIVE_PIXEL_INPUT, CH7017_HORIZONTAL_ACTIVE_PIXEL_OUTPUT,
		CH7017_VERTICAL_ACTIVE_LINE_OUTPUT, CH7017_ACTIVE_INPUT_LINE_OUTPUT,
		CH7017_LVDS_PLL_VCO_CONTROL, CH7017_LVDS_PLL_FEEDBACK_DIV,
		CH7017_LVDS_CONTROL_2, CH7017_OUTPUTS_ENABLE, CH7017_LVDS_POWER_DOWN,
	};
	int i;

	for (i = 0; i < DVO_ARRAY_SIZE(regs); i++) {
		uint8_t val = 0;

		dvo_readb(dvo, regs[i], &val);
		i915_dbg("[drm] i915: DVO: ch7017 reg 0x%02x: %02x\n", regs[i], val);
	}
}

static const struct dvo_chip_ops ch7017_ops = {
	.init = ch7017_init,
	.detect = ch7017_detect,
	.mode_valid = ch7017_mode_valid,
	.mode_set = ch7017_mode_set,
	.dpms = ch7017_dpms,
	.get_hw_state = ch7017_get_hw_state,
	.dump_regs = ch7017_dump_regs,
};

/* ---- National Semiconductor NS2501 LVDS scaler ------------------------------------- */

#define NS2501_VID 0x1305
#define NS2501_DID 0x6726

#define NS2501_VID_LO 0x00
#define NS2501_DID_LO 0x02

#define NS2501_REG8 0x08
#define NS2501_8_VEN (1 << 5)
#define NS2501_8_HEN (1 << 4)
#define NS2501_8_BPAS (1 << 2)
#define NS2501_8_PD (1 << 0)

/* The registers below are not in the data sheet; what they do was found
 * by experiment. */

/* C0: how the chip synchronises to its input */
#define NS2501_REGC0 0xc0
#define NS2501_C0_ENABLE (1 << 0)
#define NS2501_C0_HSYNC (1 << 1)
#define NS2501_C0_VSYNC (1 << 2)
#define NS2501_C0_RESET (1 << 7)

/* 41: 0x32 whenever C0 is 0x05 (hsync off), 0x00 otherwise */
#define NS2501_REG41 0x41

/* the PLL counters: 1B, and the pair 1C/1D */
#define NS2501_REG1B 0x1b
#define NS2501_REG1C 0x1c
#define NS2501_REG1D 0x1d

/* scaler factors (2^16 / value), vertical 10/11, horizontal B8/B9 */
#define NS2501_REG10 0x10
#define NS2501_REG11 0x11
#define NS2501_REGB8 0xb8
#define NS2501_REGB9 0xb9

/* the window the scaler samples the input from */
#define NS2501_REGC1 0xc1
#define NS2501_REGC2 0xc2
#define NS2501_REGC3 0xc3
#define NS2501_REGC4 0xc4
#define NS2501_REGC5 0xc5
#define NS2501_REGC6 0xc6
#define NS2501_REGC7 0xc7
#define NS2501_REGC8 0xc8

/* manual vertical sync start (ignored when the input sync is earlier) */
#define NS2501_REG80 0x80
#define NS2501_REG81 0x81

/* lines generated on the output side */
#define NS2501_REG82 0x82
#define NS2501_REG83 0x83

/* end of the front porch, horizontal (+256) and vertical */
#define NS2501_REG98 0x98
#define NS2501_REG99 0x99
#define NS2501_REG8E 0x8e
#define NS2501_REG8F 0x8f

/* DVO output and backlight enables, the same bits in both */
#define NS2501_REG34 0x34
#define NS2501_REG35 0x35
#define NS2501_34_ENABLE_OUTPUT (1 << 0)
#define NS2501_34_ENABLE_BACKLIGHT (1 << 1)

/* vertical output offset */
#define NS2501_REG9C 0x9c
#define NS2501_REG9D 0x9d

/* dithering (needs the scaler on): enable bit and depth */
#define NS2501_REGF9 0xf9

enum {
	NS2501_MODE_640x480,
	NS2501_MODE_800x600,
	NS2501_MODE_1024x768,
};

struct ns2501_reg {
	uint8_t offset;
	uint8_t value;
};

/* Configurations for a 1024x768 panel, partly what the firmware of the
 * Fujitsu Lifebook S6010 programs and partly tuned by hand. */
static const struct ns2501_configuration ns2501_modes[] = {
	[NS2501_MODE_640x480] = {
		.sync = NS2501_C0_ENABLE | NS2501_C0_VSYNC,
		.conf = NS2501_8_VEN | NS2501_8_HEN | NS2501_8_PD,
		.syncb = 0x32,
		.dither = 0x0f,
		.pll_a = 17,
		.pll_b = 852,
		.hstart = 144,
		.hstop = 783,
		.vstart = 22,
		.vstop = 514,
		.vsync = 2047, /* ignored with this configuration */
		.vtotal = 1341,
		.hpos = 0,
		.vpos = 16,
		.voffs = 36,
		.hscale = 40960,
		.vscale = 40960
	},
	[NS2501_MODE_800x600] = {
		.sync = NS2501_C0_ENABLE | NS2501_C0_HSYNC | NS2501_C0_VSYNC,
		.conf = NS2501_8_VEN | NS2501_8_HEN | NS2501_8_PD,
		.syncb = 0x00,
		.dither = 0x0f,
		.pll_a = 25,
		.pll_b = 612,
		.hstart = 215,
		.hstop = 1016,
		.vstart = 26,
		.vstop = 627,
		.vsync = 807,
		.vtotal = 1341,
		.hpos = 0,
		.vpos = 4,
		.voffs = 35,
		.hscale = 51248,
		.vscale = 51232
	},
	[NS2501_MODE_1024x768] = {
		.sync = NS2501_C0_ENABLE | NS2501_C0_VSYNC,
		.conf = NS2501_8_VEN | NS2501_8_HEN | NS2501_8_PD,
		.syncb = 0x32,
		.dither = 0x0f,
		.pll_a = 11,
		.pll_b = 1350,
		.hstart = 276,
		.hstop = 1299,
		.vstart = 15,
		.vstop = 1056,
		.vsync = 2047,
		.vtotal = 1341,
		.hpos = 0,
		.vpos = 7,
		.voffs = 27,
		.hscale = 65535,
		.vscale = 65535
	}
};

/* Values the same firmware leaves in the other registers; they do not
 * depend on the mode and their meaning is unknown. */
static const struct ns2501_reg ns2501_mode_agnostic_values[] = {
	{ 0x0a, 0x81 }, { 0x12, 0x02 }, { 0x18, 0x07 }, { 0x19, 0x00 },
	{ 0x1a, 0x00 }, { 0x1e, 0x02 }, { 0x1f, 0x40 }, { 0x20, 0x00 },
	{ 0x21, 0x00 }, { 0x22, 0x00 }, { 0x23, 0x00 }, { 0x24, 0x00 },
	{ 0x25, 0x00 }, { 0x26, 0x00 }, { 0x27, 0x00 }, { 0x7e, 0x18 },
	{ 0x84, 0x00 }, { 0x85, 0x00 }, { 0x86, 0x00 }, { 0x87, 0x00 },
	{ 0x88, 0x00 }, { 0x89, 0x00 }, { 0x8a, 0x00 }, { 0x8b, 0x00 },
	{ 0x8c, 0x10 }, { 0x8d, 0x02 }, { 0x90, 0xff }, { 0x91, 0x07 },
	{ 0x92, 0xa0 }, { 0x93, 0x02 }, { 0x94, 0x00 }, { 0x95, 0x00 },
	{ 0x96, 0x05 }, { 0x97, 0x00 }, { 0x9a, 0x88 }, { 0x9b, 0x00 },
	{ 0x9e, 0x25 }, { 0x9f, 0x03 }, { 0xa0, 0x28 }, { 0xa1, 0x01 },
	{ 0xa2, 0x28 }, { 0xa3, 0x05 },
	/* 0xa4 is mode specific, but 0x80..0x84 always works */
	{ 0xa4, 0x84 }, { 0xa5, 0x00 }, { 0xa6, 0x00 }, { 0xa7, 0x00 },
	{ 0xa8, 0x00 },
	/* 0xa9 to 0xab are mode specific, without visible effect */
	{ 0xa9, 0x04 }, { 0xaa, 0x70 }, { 0xab, 0x4f }, { 0xac, 0x00 },
	{ 0xad, 0x00 }, { 0xb6, 0x09 }, { 0xb7, 0x03 }, { 0xba, 0x00 },
	{ 0xbb, 0x20 }, { 0xf3, 0x90 }, { 0xf4, 0x00 }, { 0xf7, 0x88 },
	/* 0xf8 is mode specific, but the value does not matter */
	{ 0xf8, 0x0a }, { 0xf9, 0x00 },
};

static const struct ns2501_reg ns2501_regs_init[] = {
	{ 0x35, 0xff },
	{ 0x34, 0x00 },
	{ 0x08, 0x30 },
};

/* The chip only answers on the bus while its DVO clock runs, which is
 * why the probe turns the 2x clock on first. */
static int ns2501_init(struct dvo_dev *dvo)
{
	uint8_t ch;

	dvo->quiet = 1;
	if (!dvo_readb(dvo, NS2501_VID_LO, &ch))
		return 0;
	if (ch != (NS2501_VID & 0xff)) {
		i915_dbg("[drm] i915: DVO: ns2501 not detected, got %d from %s target %d\n",
			 ch, dvo->i2c->name, dvo->desc->addr);
		return 0;
	}
	if (!dvo_readb(dvo, NS2501_DID_LO, &ch))
		return 0;
	if (ch != (NS2501_DID & 0xff)) {
		i915_dbg("[drm] i915: DVO: ns2501 not detected, got %d from %s target %d\n",
			 ch, dvo->i2c->name, dvo->desc->addr);
		return 0;
	}
	dvo->quiet = 0;
	i915_dbg("[drm] i915: DVO: ns2501 controller found\n");
	return 1;
}

static int ns2501_detect(struct dvo_dev *dvo)
{
	/* A laptop panel without hotplug; the chip's own detection bit is
	 * unreliable anyway. */
	(void)dvo;
	return 1;
}

static int ns2501_mode_valid(struct dvo_dev *dvo, const struct drm_mode_modeinfo *mode)
{
	(void)dvo;
	i915_dbg("[drm] i915: DVO: ns2501 mode valid? (hdisplay=%d,htotal=%d,vdisplay=%d,vtotal=%d)\n",
		 mode->hdisplay, mode->htotal, mode->vdisplay, mode->vtotal);

	/* The modes there is a configuration for. */
	if ((mode->hdisplay == 640 && mode->vdisplay == 480 && mode->clock == 25175) ||
	    (mode->hdisplay == 800 && mode->vdisplay == 600 && mode->clock == 40000) ||
	    (mode->hdisplay == 1024 && mode->vdisplay == 768 && mode->clock == 65000))
		return DVO_MODE_OK;
	return DVO_MODE_ONE_SIZE;
}

static void ns2501_mode_set(struct dvo_dev *dvo, const struct drm_mode_modeinfo *mode,
			    const struct drm_mode_modeinfo *adj)
{
	const struct ns2501_configuration *conf;
	int mode_idx, i;

	i915_dbg("[drm] i915: DVO: ns2501 set mode (hdisplay=%d,htotal=%d,vdisplay=%d,vtotal=%d)\n",
		 mode->hdisplay, mode->htotal, mode->vdisplay, mode->vtotal);
	i915_dbg("[drm] i915: DVO: ns2501 timing: clock %u kHz, h %d %d %d %d, v %d %d %d %d\n",
		 adj->clock, adj->hdisplay, adj->hsync_start, adj->hsync_end, adj->htotal,
		 adj->vdisplay, adj->vsync_start, adj->vsync_end, adj->vtotal);

	if (mode->hdisplay == 640 && mode->vdisplay == 480)
		mode_idx = NS2501_MODE_640x480;
	else if (mode->hdisplay == 800 && mode->vdisplay == 600)
		mode_idx = NS2501_MODE_800x600;
	else if (mode->hdisplay == 1024 && mode->vdisplay == 768)
		mode_idx = NS2501_MODE_1024x768;
	else
		return;

	/* Done every time; it does not hurt. */
	for (i = 0; i < DVO_ARRAY_SIZE(ns2501_regs_init); i++)
		dvo_writeb(dvo, ns2501_regs_init[i].offset, ns2501_regs_init[i].value);

	for (i = 0; i < DVO_ARRAY_SIZE(ns2501_mode_agnostic_values); i++)
		dvo_writeb(dvo, ns2501_mode_agnostic_values[i].offset,
			   ns2501_mode_agnostic_values[i].value);

	conf = &ns2501_modes[mode_idx];
	dvo->ns_conf = conf;

	dvo_writeb(dvo, NS2501_REG8, conf->conf);
	dvo_writeb(dvo, NS2501_REG1B, conf->pll_a);
	dvo_writeb(dvo, NS2501_REG1C, conf->pll_b & 0xff);
	dvo_writeb(dvo, NS2501_REG1D, conf->pll_b >> 8);
	dvo_writeb(dvo, NS2501_REGC1, conf->hstart & 0xff);
	dvo_writeb(dvo, NS2501_REGC2, conf->hstart >> 8);
	dvo_writeb(dvo, NS2501_REGC3, conf->hstop & 0xff);
	dvo_writeb(dvo, NS2501_REGC4, conf->hstop >> 8);
	dvo_writeb(dvo, NS2501_REGC5, conf->vstart & 0xff);
	dvo_writeb(dvo, NS2501_REGC6, conf->vstart >> 8);
	dvo_writeb(dvo, NS2501_REGC7, conf->vstop & 0xff);
	dvo_writeb(dvo, NS2501_REGC8, conf->vstop >> 8);
	dvo_writeb(dvo, NS2501_REG80, conf->vsync & 0xff);
	dvo_writeb(dvo, NS2501_REG81, conf->vsync >> 8);
	dvo_writeb(dvo, NS2501_REG82, conf->vtotal & 0xff);
	dvo_writeb(dvo, NS2501_REG83, conf->vtotal >> 8);
	dvo_writeb(dvo, NS2501_REG98, conf->hpos & 0xff);
	dvo_writeb(dvo, NS2501_REG99, conf->hpos >> 8);
	dvo_writeb(dvo, NS2501_REG8E, conf->vpos & 0xff);
	dvo_writeb(dvo, NS2501_REG8F, conf->vpos >> 8);
	dvo_writeb(dvo, NS2501_REG9C, conf->voffs & 0xff);
	dvo_writeb(dvo, NS2501_REG9D, conf->voffs >> 8);
	dvo_writeb(dvo, NS2501_REGB8, conf->hscale & 0xff);
	dvo_writeb(dvo, NS2501_REGB9, conf->hscale >> 8);
	dvo_writeb(dvo, NS2501_REG10, conf->vscale & 0xff);
	dvo_writeb(dvo, NS2501_REG11, conf->vscale >> 8);
	dvo_writeb(dvo, NS2501_REGF9, conf->dither);
	dvo_writeb(dvo, NS2501_REG41, conf->syncb);
	dvo_writeb(dvo, NS2501_REGC0, conf->sync);
}

static int ns2501_get_hw_state(struct dvo_dev *dvo)
{
	uint8_t ch;

	if (!dvo_readb(dvo, NS2501_REG8, &ch))
		return 0;
	return (ch & NS2501_8_PD) ? 1 : 0;
}

static void ns2501_dpms(struct dvo_dev *dvo, int enable)
{
	const struct ns2501_configuration *conf = dvo->ns_conf;

	i915_dbg("[drm] i915: DVO: ns2501 dpms %d\n", enable);

	if (enable) {
		/* Nothing to turn on without a programmed mode. */
		if (!conf)
			return;
		dvo_writeb(dvo, NS2501_REGC0, conf->sync | 0x08);
		dvo_writeb(dvo, NS2501_REG41, conf->syncb);
		dvo_writeb(dvo, NS2501_REG34, NS2501_34_ENABLE_OUTPUT);
		lg_mdelay(15);

		dvo_writeb(dvo, NS2501_REG8, conf->conf | NS2501_8_BPAS);
		if (!(conf->conf & NS2501_8_BPAS))
			dvo_writeb(dvo, NS2501_REG8, conf->conf);
		lg_mdelay(200);

		dvo_writeb(dvo, NS2501_REG34,
			   NS2501_34_ENABLE_OUTPUT | NS2501_34_ENABLE_BACKLIGHT);
		dvo_writeb(dvo, NS2501_REGC0, conf->sync);
	} else {
		dvo_writeb(dvo, NS2501_REG34, NS2501_34_ENABLE_OUTPUT);
		lg_mdelay(200);

		dvo_writeb(dvo, NS2501_REG8, NS2501_8_VEN | NS2501_8_HEN | NS2501_8_BPAS);
		lg_mdelay(15);

		dvo_writeb(dvo, NS2501_REG34, 0x00);
	}
}

static const struct dvo_chip_ops ns2501_ops = {
	.init = ns2501_init,
	.detect = ns2501_detect,
	.mode_valid = ns2501_mode_valid,
	.mode_set = ns2501_mode_set,
	.dpms = ns2501_dpms,
	.get_hw_state = ns2501_get_hw_state,
};

/* ---- where the chips sit ------------------------------------------------------------ */

#define SIL164_ADDR 0x38
#define CH7xxx_ADDR 0x76
#define TFP410_ADDR 0x38
#define NS2501_ADDR 0x38

/* Tried in this order; the first that answers is the one. */
static const struct dvo_device_desc dvo_devices[] = {
	{ DVO_CHIP_TMDS, "sil164", LG_PORT_C, 0, SIL164_ADDR, &sil164_ops },
	{ DVO_CHIP_TMDS, "ch7xxx", LG_PORT_C, 0, CH7xxx_ADDR, &ch7xxx_ops },
	/* some CH7010s */
	{ DVO_CHIP_TMDS, "ch7xxx", LG_PORT_C, 0, 0x75, &ch7xxx_ops },
	/* might also be at 0x44, 0x84 or 0xc4 */
	{ DVO_CHIP_LVDS, "ivch", LG_PORT_A, 0, 0x02, &ivch_ops },
	{ DVO_CHIP_TMDS, "tfp410", LG_PORT_C, 0, TFP410_ADDR, &tfp410_ops },
	{ DVO_CHIP_LVDS, "ch7017", LG_PORT_C, LG_GMBUS_PIN_DPB, 0x75, &ch7017_ops },
	{ DVO_CHIP_LVDS_NO_FIXED, "ns2501", LG_PORT_B, 0, NS2501_ADDR, &ns2501_ops },
};

static struct dvo_dev *dvo_of(struct lg_output *o)
{
	return (struct dvo_dev *)o->priv;
}

/* The GPIO pair behind a GMBUS pin on the GMCH parts. */
static int dvo_pin_to_gpio(unsigned pin)
{
	static const int map[LG_GMBUS_NUM_PINS] = {
		[LG_GMBUS_PIN_SSC] = 1, /* GPIOB */
		[LG_GMBUS_PIN_VGADDC] = 0, /* GPIOA */
		[LG_GMBUS_PIN_PANEL] = 2, /* GPIOC */
		[LG_GMBUS_PIN_DPC] = 3, /* GPIOD */
		[LG_GMBUS_PIN_DPB] = 4, /* GPIOE */
		[LG_GMBUS_PIN_DPD] = 5, /* GPIOF */
	};

	if (pin == LG_GMBUS_PIN_DISABLED || pin >= LG_GMBUS_NUM_PINS)
		return -1;
	return map[pin];
}

/* drm_mode_vrefresh: Hz, rounded, for interlace, double scan and vscan. */
static int dvo_mode_vrefresh(const struct drm_mode_modeinfo *m)
{
	uint64_t num, den;
	int refresh;

	if (!m->htotal || !m->vtotal)
		return 0;
	num = (uint64_t)m->clock * 1000;
	den = (uint64_t)m->htotal * m->vtotal;
	if (m->flags & DRM_MODE_FLAG_INTERLACE)
		num *= 2;
	if (m->flags & DRM_MODE_FLAG_DBLSCAN)
		den *= 2;
	if (m->vscan > 1)
		den *= m->vscan;
	refresh = (int)((num + den / 2) / den);
	return refresh;
}

/* ---- the output hooks ------------------------------------------------------------- */

static int dvo_detect(struct lg_display *d, struct lg_output *o)
{
	struct dvo_dev *dvo = dvo_of(o);
	int connected;

	(void)d;
	connected = dvo->desc->ops->detect(dvo);
	i915_dbg("[drm] i915: %s: %s %s\n", o->name, dvo->desc->name,
		 connected ? "connected" : "disconnected");
	return connected;
}

static int dvo_get_modes(struct lg_display *d, struct lg_output *o, int conn)
{
	int n;

	/* The monitor's EDID when the DDC answers; a panel without one shows
	 * its fixed mode. */
	if (!o->fixed_mode_valid)
		return lg_get_modes_edid(d, o, conn, d->max_dotclk_khz);

	if (lg_read_edid(d, o) == 0 && o->edid_len > 0) {
		n = drm_connector_set_edid(d->drm, conn, o->edid, (unsigned)o->edid_len);
		if (n > 0)
			return n;
	}
	if (drm_connector_add_mode(d->drm, conn, &o->fixed_mode) < 0)
		return 0;
	return 1;
}

static int dvo_mode_valid(struct lg_display *d, struct lg_output *o,
			  const struct drm_mode_modeinfo *m)
{
	struct dvo_dev *dvo = dvo_of(o);
	uint32_t target_clock = m->clock;
	int status;

	if (m->flags & DRM_MODE_FLAG_DBLSCAN)
		return -EINVAL;

	/* A panel takes its own timing only. */
	if (o->fixed_mode_valid) {
		const struct drm_mode_modeinfo *f = &o->fixed_mode;

		if (m->hdisplay != f->hdisplay || m->vdisplay != f->vdisplay ||
		    dvo_mode_vrefresh(m) != dvo_mode_vrefresh(f))
			return DVO_MODE_PANEL;
		target_clock = f->clock;
	}

	if (d->max_dotclk_khz && target_clock > d->max_dotclk_khz)
		return DVO_MODE_CLOCK_HIGH;

	status = dvo->desc->ops->mode_valid(dvo, m);
	return status;
}

static int dvo_compute_config(struct lg_display *d, struct lg_output *o,
			      struct lg_config *cfg)
{
	(void)d;

	/* A panel runs its own timing; the chip scales the picture, the
	 * pipe's fitter is not used. */
	if (o->fixed_mode_valid) {
		int vrefresh = dvo_mode_vrefresh(&cfg->mode);
		int fixed_vrefresh = dvo_mode_vrefresh(&o->fixed_mode);

		if (vrefresh - fixed_vrefresh > 1 || fixed_vrefresh - vrefresh > 1) {
			i915_dbg("[drm] i915: %s: requested %d Hz does not match the panel's %d Hz\n",
				 o->name, vrefresh, fixed_vrefresh);
			return -EINVAL;
		}
		cfg->mode = o->fixed_mode;
	}

	if (cfg->mode.flags & DRM_MODE_FLAG_DBLSCAN)
		return -EINVAL;

	/* The core derives the DVO 2x clock from the output type. */
	cfg->port_clock = cfg->mode.clock;
	return 0;
}

static void dvo_pre_enable(struct lg_display *d, struct lg_output *o,
			   const struct lg_config *cfg)
{
	uint32_t dvo_val;
	int port = o->port;

	/* Keep the active data order as found: what it should be is not
	 * known. */
	dvo_val = lg_rd(d, DVO_REG(port)) &
		  (DVO_DEDICATED_INT_ENABLE | DVO_PRESERVE_MASK | LG_DVO_ACT_DATA_ORDER_MASK);
	dvo_val |= DVO_DATA_ORDER_FP | DVO_BORDER_ENABLE | DVO_BLANK_ACTIVE_HIGH;
	dvo_val |= DVO_PIPE_SEL(cfg->pipe);
	dvo_val |= DVO_PIPE_STALL;
	if (cfg->mode.flags & DRM_MODE_FLAG_PHSYNC)
		dvo_val |= DVO_HSYNC_ACTIVE_HIGH;
	if (cfg->mode.flags & DRM_MODE_FLAG_PVSYNC)
		dvo_val |= DVO_VSYNC_ACTIVE_HIGH;

	lg_wr(d, DVO_SRCDIM(port),
	      DVO_SRCDIM_HORIZONTAL(cfg->t.hdisplay) | DVO_SRCDIM_VERTICAL(cfg->t.vdisplay));
	lg_wr(d, DVO_REG(port), dvo_val);
}

static void dvo_enable(struct lg_display *d, struct lg_output *o,
		       const struct lg_config *cfg)
{
	struct dvo_dev *dvo = dvo_of(o);
	struct drm_mode_modeinfo mode = cfg->mode;

	/* What the client asked for (its picture size) and what is sent. */
	if (cfg->src_w && cfg->src_h) {
		mode.hdisplay = (uint16_t)cfg->src_w;
		mode.vdisplay = (uint16_t)cfg->src_h;
	}
	dvo->desc->ops->mode_set(dvo, &mode, &cfg->mode);

	lg_rmw(d, DVO_REG(o->port), 0, DVO_ENABLE);
	lg_posting_read(d, DVO_REG(o->port));

	dvo->desc->ops->dpms(dvo, 1);
}

static void dvo_disable(struct lg_display *d, struct lg_output *o,
			const struct lg_config *cfg)
{
	struct dvo_dev *dvo = dvo_of(o);

	(void)cfg;
	dvo->desc->ops->dpms(dvo, 0);

	lg_rmw(d, DVO_REG(o->port), DVO_ENABLE, 0);
	lg_posting_read(d, DVO_REG(o->port));
}

static int dvo_get_hw_state(struct lg_display *d, struct lg_output *o, int *pipe)
{
	uint32_t tmp = lg_rd(d, DVO_REG(o->port));

	if (pipe)
		*pipe = (tmp & DVO_PIPE_SEL_MASK) ? 1 : 0;
	return (tmp & DVO_ENABLE) ? 1 : 0;
}

static const struct lg_output_funcs dvo_funcs = {
	.detect = dvo_detect,
	.get_modes = dvo_get_modes,
	.mode_valid = dvo_mode_valid,
	.compute_config = dvo_compute_config,
	.pre_enable = dvo_pre_enable,
	.enable = dvo_enable,
	.disable = dvo_disable,
	.get_hw_state = dvo_get_hw_state,
};

/* ---- probing ------------------------------------------------------------------------ */

static int dvo_platform_has_port(struct lg_display *d, int port)
{
	if (port == LG_PORT_A)
		return d->is_i830;
	return port == LG_PORT_B || port == LG_PORT_C;
}

/* Try one table entry: its bus, the chip's ids. */
static int dvo_init_dev(struct lg_display *d, struct dvo_dev *dvo,
			const struct dvo_device_desc *desc)
{
	uint32_t dpll[LG_MAX_PIPES];
	unsigned pin;
	int gpio, pipe, ret;

	/* The table may name the bus; otherwise everything is on GPIOE
	 * except the panels of i830 laptops, which are on GPIOB. */
	if (desc->gpio != LG_GMBUS_PIN_DISABLED && lg_gmbus_pin_valid(d, desc->gpio))
		pin = desc->gpio;
	else if (desc->type == DVO_CHIP_LVDS)
		pin = LG_GMBUS_PIN_SSC;
	else
		pin = LG_GMBUS_PIN_DPB;

	mm_memset(dvo, 0, sizeof(*dvo));
	dvo->d = d;
	dvo->desc = desc;
	dvo->pin = pin;
	dvo->i2c = lg_gmbus_adapter(d, pin);
	gpio = dvo_pin_to_gpio(pin);
	if (!dvo->i2c || gpio < 0)
		return 0;
	dvo->gpio_reg = GPIO_CTL(d->gmbus_base, (uint32_t)gpio);

	/* The controller's NAK handling is not reliable enough for probing
	 * absent chips: bit-bang the bus meanwhile. */
	lg_gmbus_force_bit(d, pin, 1);

	/* The NS2501 only answers while the DVO 2x clock runs. */
	for (pipe = 0; pipe < d->num_pipes && pipe < LG_MAX_PIPES; pipe++) {
		dpll[pipe] = lg_rd(d, DPLL(d, pipe));
		lg_wr(d, DPLL(d, pipe), dpll[pipe] | DPLL_DVO_2X_MODE);
	}

	ret = desc->ops->init(dvo);

	/* The 2x clocks back as they were. */
	for (pipe = 0; pipe < d->num_pipes && pipe < LG_MAX_PIPES; pipe++)
		lg_wr(d, DPLL(d, pipe), dpll[pipe]);

	/* The i830's GMBUS does not work at all: its buses stay bit-banged. */
	if (!d->is_i830)
		lg_gmbus_force_bit(d, pin, 0);

	return ret;
}

/* The mode the firmware left the panel in: the pipe the port runs on, read
 * back, with the port's sync polarities. */
static void dvo_read_fixed_mode(struct lg_display *d, struct lg_output *o)
{
	struct drm_mode_modeinfo m;
	uint32_t tmp;
	int pipe;

	if (!dvo_get_hw_state(d, o, &pipe)) {
		i915_dbg("[drm] i915: %s: port off, no panel mode\n", o->name);
		return;
	}
	mm_memset(&m, 0, sizeof(m));
	if (lg_pipe_read_mode(d, pipe, &m) < 0 || !m.clock || !m.hdisplay || !m.vdisplay)
		return;

	tmp = lg_rd(d, DVO_REG(o->port));
	m.flags &= ~(DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NHSYNC |
		     DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_NVSYNC);
	m.flags |= (tmp & DVO_HSYNC_ACTIVE_HIGH) ? DRM_MODE_FLAG_PHSYNC : DRM_MODE_FLAG_NHSYNC;
	m.flags |= (tmp & DVO_VSYNC_ACTIVE_HIGH) ? DRM_MODE_FLAG_PVSYNC : DRM_MODE_FLAG_NVSYNC;
	m.type |= DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER;
	if (!m.vrefresh)
		m.vrefresh = (uint32_t)dvo_mode_vrefresh(&m);
	if (!m.name[0])
		ksnprintf(m.name, sizeof(m.name), "%dx%d", m.hdisplay, m.vdisplay);

	o->fixed_mode = m;
	o->fixed_mode_valid = 1;
	i915_dbg("[drm] i915: %s: current (BIOS) panel mode %ux%u@%u, %u kHz\n", o->name,
		 m.hdisplay, m.vdisplay, m.vrefresh, m.clock);
}

void lg_dvo_init(struct lg_display *d)
{
	struct dvo_dev *dvo;
	struct lg_output *o;
	const struct dvo_device_desc *desc = NULL;
	int i;

	if (d->ver != 2)
		return;

	dvo = kalloc(sizeof(*dvo));
	if (!dvo)
		return;

	/* The i830's GMBUS is unusable; the DDC of the DVO port too. */
	if (d->is_i830)
		lg_gmbus_force_bit(d, LG_GMBUS_PIN_DPC, 1);

	for (i = 0; i < DVO_ARRAY_SIZE(dvo_devices); i++) {
		if (!dvo_platform_has_port(d, dvo_devices[i].port))
			continue;
		if (dvo_init_dev(d, dvo, &dvo_devices[i])) {
			desc = &dvo_devices[i];
			break;
		}
	}
	if (!desc) {
		i915_dbg("[drm] i915: DVO: no encoder chip found\n");
		kfree(dvo);
		return;
	}

	o = lg_output_new(d);
	if (!o) {
		kprintf("[drm] i915: DVO: no free output slot for %s\n", desc->name);
		kfree(dvo);
		return;
	}

	o->type = LG_OUTPUT_DVO;
	o->port = desc->port;
	o->reg = DVO_REG(desc->port);
	ksnprintf(o->name, sizeof(o->name), "DVO-%c", 'A' + desc->port);
	o->funcs = &dvo_funcs;
	o->pipe_mask = (1u << d->num_pipes) - 1;
	o->hpd_pin = LG_HPD_NONE;
	o->priv = dvo;
	/* The monitor's DDC is on GPIOD whatever the chip. */
	o->ddc_pin = LG_GMBUS_PIN_DPC;
	o->ddc = lg_gmbus_adapter(d, LG_GMBUS_PIN_DPC);

	if (desc->type == DVO_CHIP_TMDS) {
		o->conn_type = DRM_MODE_CONNECTOR_DVII;
		o->enc_type = DRM_MODE_ENCODER_TMDS;
		o->polled = 1;
	} else {
		o->conn_type = DRM_MODE_CONNECTOR_LVDS;
		o->enc_type = DRM_MODE_ENCODER_LVDS;
		o->is_panel = 1;
		/* The panel's timing is not in the VBT in a form we read for
		 * these chips: take what the firmware runs on the port.  The
		 * NS2501 has no fixed mode; it scales its three modes itself. */
		if (desc->type == DVO_CHIP_LVDS)
			dvo_read_fixed_mode(d, o);
	}

	kprintf("[drm] i915: %s: %s %s encoder on %s\n", o->name, desc->name,
		desc->type == DVO_CHIP_TMDS ? "DVI" : "LVDS", dvo->i2c->name);
	/* The chip's registers as the firmware left them (debug builds:
	 * every one is a bus transaction). */
	if (I915_DEBUG && desc->ops->dump_regs)
		desc->ops->dump_regs(dvo);
}
