// LikeOS Cirrus Logic CS42L43 codec on SoundWire.
//
// The CS42L43 is the jack codec of many recent laptops (the Dell Pro and
// Latitude Arrow Lake machines among them): headphone/headset jack with
// detection, a stereo class-D speaker amplifier, digital microphone inputs
// and an internal mixer, all behind one SoundWire peripheral.  Its
// registers are 32 bits wide, addressed in steps of four, and reach the
// bus as four consecutive byte addresses (paged above 0x8000).
//
// Bring-up: the device carries a small microcontroller that runs from ROM
// at power-on.  Depending on its revision it must be patched (the
// "cs42l43.bin" firmware), advanced to its run stage, or disabled so the
// host owns the registers; every step may end in a soft reset, after
// which the device drops off the bus and re-enumerates.  Then a register
// patch, interrupt masks, jack detection set-up and defaults.
//
// Data ports: DP5 (headphone path), DP6 (speaker path), DP7 (spare)
// playback; DP1 (headset microphone, up to 4 channels) and DP2 (digital
// microphones) capture.  The internal mixer routes DP5/DP6 to the speaker
// (AMP1/AMP2) and headphone (AMP3/AMP4) outputs; with only one playback
// stream both outputs take it and the routing decides what sounds.
//
// The audio clocks come from a PLL locked to the SoundWire bus clock; it
// runs while any stream is prepared.  Amplifier power-up and power-down
// complete asynchronously and are signalled through interrupt status bits,
// which we poll (they latch whether masked or not) and also collect from
// the SoundWire alert path, so neither depends on the other.
//
// The speaker digital volume is limited to 0 dB (register value 128):
// above that the amplifier clips and can damage small laptop speakers.
//
// Copyright (C) 2026 The LikeOS Project

#include "sdw.h"
#include <kernel/mm/memory.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/firmware.h>

/* ==========================================================================
 * Registers
 * ========================================================================== */

#define CS_GEN_INT_STAT_1 0x000000C0 /* byte register on the SoundWire map */
#define CS_GEN_INT_MASK_1 0x000000C1
#define CS_INT_STAT_GEN1 0x01

#define CS_DEVID 0x00003000
#define CS_REVID 0x00003004
#define CS_RELID 0x0000300C
#define CS_SFT_RESET 0x00003020
#define CS_SFT_RESET_VAL 0x5A000000u
#define CS_MCLK_SRC_SEL 0x00007004
#define CS_OSC_PLL_MCLK_SEL 0x1u
#define CS_CCM_BLK_CLK_CONTROL 0x00007010
#define CS_SAMPLE_RATE1 0x00007014
#define CS_SAMPLE_RATE_MASK 0x1fu
#define CS_PLL_CONTROL 0x00007034
#define CS_PLL_REFCLK_EN 0x8u
#define CS_PLL_REFCLK_DIV_MASK 0x6u
#define CS_PLL_REFCLK_SRC_MASK 0x1u
#define CS_PLL_SRC_SDW 1u
#define CS_OSC_DIV_SEL 0x00007068
#define CS_OSC_DIV2_EN 0x1u
#define CS_ADC_B_CTRL1 0x00008000
#define CS_ADC_B_CTRL2 0x00008004
#define CS_PGA_WIDESWING 0x80u
#define CS_DECIM_VOL_CTRL_CH1_CH2 0x00008050
#define CS_DECIM_VOL_CTRL_CH3_CH4 0x00008054
#define CS_DECIM_VOL_CTRL_UPDATE 0x00008058
#define CS_B_DECIM_VOL_CTRL_CH1_CH2 0x00008280
#define CS_B_DECIM_VOL_CTRL_CH3_CH4 0x00008284
#define CS_B_DECIM_VOL_CTRL_UPDATE 0x0000829C
#define CS_DECIM_LO_MUTE 0x00008000u /* first decimator of a pair */
#define CS_DECIM_LO_VOL 0x00003FC0u
#define CS_DECIM_HI_MUTE 0x80000000u
#define CS_DECIM_HI_VOL 0x3FC00000u
#define CS_INTP_VOLUME_CTRL1 0x00009008
#define CS_INTP_VOLUME_CTRL2 0x0000900C
#define CS_AMP1_2_VU 0x200u
#define CS_AMP_MUTE 0x100u
#define CS_AMP_VOL 0xffu
#define CS_SPK_VOL_0DB 128u /* the limit: 0 dB */
#define CS_SWIRE_DP1_CH1_INPUT 0x0000C280
#define CS_SWIRE_DP1_CH2_INPUT 0x0000C290
#define CS_SWIRE_DP2_CH1_INPUT 0x0000C2C0
#define CS_SWIRE_DP2_CH2_INPUT 0x0000C2D0
#define CS_AMP1MIX_INPUT1 0x0000C620
#define CS_AMP2MIX_INPUT1 0x0000C630
#define CS_AMP3MIX_INPUT1 0x0000C640
#define CS_AMP4MIX_INPUT1 0x0000C650
#define CS_MIXER_SRC_MASK 0x1ffu
#define CS_MIXER_VOL_MASK 0x00FE0000u
#define CS_MIXER_VOL_0DB 0x40u
#define CS_PDNCNTL 0x00010000
#define CS_RING_SENSE_EN 0x2u
#define CS_RINGSENSE_DEB_CTRL 0x0001001C
#define CS_TIPSENSE_DEB_CTRL 0x00010020
#define CS_TIPSENSE_INV 0x80u
#define CS_TIPSENSE_FALL_DB 0x38u
#define CS_TIPSENSE_RISE_DB 0x07u
#define CS_TIP_RING_SENSE_STATUS 0x00010028
#define CS_TIPSENSE_PLUG_DB_STS_SHIFT 2
#define CS_RINGSENSE_PLUG_DB_STS_SHIFT 0
#define CS_HS2 0x00010040
#define CS_HS_CLAMP_DISABLE 0x10000000u
#define CS_HSBIAS_RAMP 0x0C000000u
#define CS_HSDET_MODE 0x00018000u
#define CS_HSDET_MANUAL_MODE 0x00006000u
#define CS_AUTO_HSDET_TIME 0x00000700u
#define CS_HS_STAT 0x00010048
#define CS_HSDET_TYPE_STS 0x7u
#define CS_MCU_SW_INTERRUPT 0x00010094
#define CS_CONTROL_IND 0x4u
#define CS_PATCH_IND 0x1u
#define CS_STEREO_MIC_CTRL 0x000100A4
#define CS_JACK_STEREO_CONFIG 0x3u
#define CS_HS1_BIAS_SENSE_EN 0x10u
#define CS_STEREO_MIC_CLAMP_CTRL 0x000100C4
#define CS_SMIC_HPAMP_CLAMP_DIS_FRC_VAL 0x2u
#define CS_SMIC_HPAMP_CLAMP_DIS_FRC 0x1u
#define CS_BLOCK_EN3 0x00010108
#define CS_ADC1_EN 0x1u
#define CS_ADC2_EN 0x2u
#define CS_PDM1_DIN_L_EN 0x4u
#define CS_PDM1_DIN_R_EN 0x8u
#define CS_BLOCK_EN6 0x00010114
#define CS_MIXER_EN 0x1u
#define CS_BLOCK_EN8 0x0001011C
#define CS_HP_EN 0x1u
#define CS_BLOCK_EN10 0x00010124
#define CS_AMP1_EN 0x1u
#define CS_AMP2_EN 0x2u
#define CS_MIC_DETECT_CONTROL_1 0x00011074
#define CS_BUTTON_DETECT_MODE 0x18u
#define CS_HSBIAS_MODE 0x6u
#define CS_MIC_LVL_DET_DISABLE 0x1u
#define CS_DETECT_STATUS_1 0x0001107C
#define CS_HS_BIAS_SENSE_AUTOCTL 0x00011090
#define CS_JACKDET_MODE 0xC0000000u
#define CS_JACKDET_INV 0x20000000u
#define CS_S0_AUTO_ADCMUTE_DISABLE 0x00800000u
#define CS_HSBIAS_SENSE_EN 0x80u
#define CS_AUTO_HSBIAS_CLAMP_EN 0x40u
#define CS_HSBIAS_SENSE_TRIP 0x7u
#define CS_MIC_DETECT_CONTROL_ANDROID 0x000110B0
#define CS_CTRL_REG 0x00014000
#define CS_PLL_MODE_BYPASS_500 0x4u
#define CS_PLL_MODE_BYPASS_1029 0x2u
#define CS_PLL_EN 0x1u
#define CS_FDIV_FRAC 0x00014004
#define CS_CAL_RATIO 0x00014008
#define CS_PLL_CAL_RATIO 0xffu
#define CS_DACCNFG1 0x00017000
#define CS_HP_MSTR_VOL_CTRL_EN 0x8u
#define CS_DACCNFG2 0x00017004
#define CS_HP_HPF_EN 0x1u
#define CS_HPPATHVOL 0x0001700C
#define CS_AMP3_PATH_VOL 0x000001FFu
#define CS_AMP4_PATH_VOL 0x01FF0000u
#define CS_HP_VOL_MUTE 0x11Bu /* -114.5 dB, the muted end */
#define CS_PGAVOL 0x00017014
#define CS_HP_DIG_VOL_RAMP 0xF00u
#define CS_HP_ANA_VOL_RAMP 0x00Fu
#define CS_LOADDETRESULTS 0x00017018
#define CS_AMP3_RES_DET 0x3u
#define CS_LOADDETENA 0x00017024
#define CS_HPLOAD_DET_EN 0x1u
#define CS_HP_CTRL 0x00017028
#define CS_ADPTPWR_MODE 0x7u

/* interrupt status (write 1 to clear) and masks (1 = masked) */
#define CS_DECIM_INT 0x0001B000
#define CS_PLL_INT 0x0001B00C
#define CS_SOFT_INT 0x0001B010
#define CS_MSM_INT 0x0001B018
#define CS_ACC_DET_INT 0x0001B01C
#define CS_CLASS_D_AMP_INT 0x0001B030
#define CS_HPOUT_INT 0x0001B03C
#define CS_PLL_MASK 0x0001B0AC
#define CS_MSM_MASK 0x0001B0B8
#define CS_ACC_DET_MASK 0x0001B0BC
#define CS_CLASS_D_AMP_MASK 0x0001B0D0
#define CS_HPOUT_MASK 0x0001B0DC
#define CS_SOFT_INT_SHADOW 0x0001B310

#define CS_PLL_LOST_LOCK 0x2u
#define CS_PLL_READY 0x1u
#define CS_HP_STARTUP_DONE 0x800u
#define CS_HP_SHUTDOWN_DONE 0x400u
#define CS_HSDET_DONE 0x200u
#define CS_TIPSENSE_UNPLUG_DB 0x80u
#define CS_TIPSENSE_PLUG_DB 0x40u
#define CS_RINGSENSE_UNPLUG_DB 0x20u
#define CS_RINGSENSE_PLUG_DB 0x10u
#define CS_AMP2_CLK_STOP_FAULT 0x2000u
#define CS_AMP1_CLK_STOP_FAULT 0x1000u
#define CS_AMP2_VDDSPK_FAULT 0x800u
#define CS_AMP1_VDDSPK_FAULT 0x400u
#define CS_AMP2_SHUTDOWN_DONE 0x200u
#define CS_AMP1_SHUTDOWN_DONE 0x100u
#define CS_AMP2_STARTUP_DONE 0x80u
#define CS_AMP1_STARTUP_DONE 0x40u
#define CS_AMP2_THERM_SHDN 0x20u
#define CS_AMP1_THERM_SHDN 0x10u
#define CS_AMP2_THERM_WARN 0x8u
#define CS_AMP1_THERM_WARN 0x4u
#define CS_AMP2_SCDET 0x2u
#define CS_AMP1_SCDET 0x1u
#define CS_HP_ILIMIT 0x2u
#define CS_HP_LOADDET_DONE 0x1u
#define CS_CONTROL_APPLIED 0x10u
#define CS_PATCH_APPLIED 0x1u

/* microcontroller boot */
#define CS_BOOT_CONTROL 0x00101000
#define CS_LOCK_HW_STS 0x2u
#define CS_MCU_SW_REV 0x00114000
#define CS_PATCH_START_ADDR 0x00114004
#define CS_NEED_CONFIGS 0x0011400C
#define CS_BOOT_STATUS 0x0011401C
#define CS_FW_SH_BOOT_CFG_NEED_CONFIGS 0x0011F8F8
#define CS_FW_MISSION_NEED_CONFIGS 0x0011FE00
#define CS_FW_MISSION_HAVE_CONFIGS 0x0011FE04
#define CS_FW_MISSION_MM_CTRL_SELECTION 0x0011FE0C
#define CS_FW_MISSION_MM_MCU_CFG_REG 0x0011FE10
#define CS_B_MCU_SW_REV 0x00117314
#define CS_B_PATCH_START_ADDR 0x00117318
#define CS_B_NEED_CONFIGS 0x00117320
#define CS_B_BOOT_STATUS 0x00117330
#define CS_B_FW_MISSION_MM_CTRL_SELECTION 0x00117E0C
#define CS_B_FW_MISSION_MM_MCU_CFG_REG 0x00117E10
#define CS_FW_PATCH_NEED_CFG 0x80000000u
#define CS_FW_MM_CTRL_MCU_SEL 0x1u
#define CS_FW_MM_MCU_CFG_DISABLE 0xF05AA50Fu
#define CS_MCU_UPDATE_OFFSET 0x100000u
#define CS_MCU_UPDATE_FORMAT 3

#define CS_DEVID_A 0x00042A43u
#define CS_DEVID_B 0x0042A43Bu

#define CS_MCU_ROM_REV 0x2001
#define CS_MCU_ROM_BIOS_REV 0x0000
#define CS_MCU_SUPPORTED_REV 0x2105
#define CS_MCU_SHADOW_REGS_REV 0x2200
#define CS_BIOS_SHADOW_REGS_REV 0x1002
#define CS_MCU_SUPPORTED_BIOS_REV 0x0001
#define CS_STAGE2 2
#define CS_STAGE3 3
#define CS_STAGE4 4

/* mixer sources */
#define CS_SRC_NONE 0x00
#define CS_SRC_DECIM1 0x10
#define CS_SRC_DECIM2 0x11
#define CS_SRC_DECIM3 0x12
#define CS_SRC_DECIM4 0x13
#define CS_SRC_DP5RX1 0x28
#define CS_SRC_DP5RX2 0x29
#define CS_SRC_DP6RX1 0x2A
#define CS_SRC_DP6RX2 0x2B
#define CS_SRC_DP7RX1 0x2C
#define CS_SRC_DP7RX2 0x2D

/* data ports */
#define CS_DP_HS_MIC 1
#define CS_DP_DMIC 2
#define CS_DP_HP 5
#define CS_DP_SPK 6
#define CS_DP_SPARE 7

#define CS_PLL_TIMEOUT_MS 200
#define CS_SPK_TIMEOUT_MS 100
#define CS_HP_TIMEOUT_MS 2000
#define CS_LOAD_TIMEOUT_MS 1000
#define CS_FW_MAX_BYTES (256u * 1024u)

/* ==========================================================================
 * State
 * ========================================================================== */

enum cs_irqreg {
	CS_IRQ_PLL,
	CS_IRQ_MSM,
	CS_IRQ_ACC,
	CS_IRQ_AMP,
	CS_IRQ_HPOUT,
	CS_IRQ_NREG
};

static const uint32_t g_irq_regs[CS_IRQ_NREG] = { CS_PLL_INT, CS_MSM_INT,
						  CS_ACC_DET_INT,
						  CS_CLASS_D_AMP_INT,
						  CS_HPOUT_INT };

struct cs42l43 {
	struct sdw_slave *s;
	struct snd_mutex lock;
	uint32_t devid, revid, otp;
	uint32_t mcu_rev, bios_rev;
	int variant_b;
	int booted;
	int hw_lock;

	/* clocking and streams */
	int pll_on;
	uint32_t pll_freq;
	uint32_t rate;
	uint32_t play_ports; /* bit n: playback data port n prepared */
	uint32_t cap_ports;

	/* outputs */
	int spk_on, hp_on; /* routing wants */
	int spk_powered, hp_powered;
	uint32_t spk_vol[2], hp_vol[2], cap_vol;
	int spk_mute, hp_mute, cap_mute, master_mute;
	uint32_t master_vol;

	/* jack */
	int jack_present;
	uint32_t jack_report;
	int headset;
	volatile int jack_work_queued;
	volatile uint32_t irq_seen[CS_IRQ_NREG];
	uint32_t last_trs;
	uint32_t faults;
	int detecting;
};

static inline struct cs42l43 *cs_of(struct sdw_slave *s)
{
	return (struct cs42l43 *)s->codec;
}

/* ==========================================================================
 * Register helpers
 * ========================================================================== */

static int cs_rd(struct cs42l43 *cs, uint32_t reg, uint32_t *v)
{
	return sdw_read32(cs->s, reg, v);
}

static int cs_wr(struct cs42l43 *cs, uint32_t reg, uint32_t v)
{
	return sdw_write32(cs->s, reg, v);
}

static int cs_upd(struct cs42l43 *cs, uint32_t reg, uint32_t mask, uint32_t v)
{
	return sdw_update32(cs->s, reg, mask, v);
}

/* Poll a register until (v & mask) == want. */
static int cs_poll(struct cs42l43 *cs, uint32_t reg, uint32_t mask,
		   uint32_t want, uint32_t timeout_ms, uint32_t step_ms,
		   uint32_t *last)
{
	uint64_t end = hrtimer_now_ns() + (uint64_t)timeout_ms * 1000000ull;
	uint32_t v = 0;

	for (;;) {
		int rc = cs_rd(cs, reg, &v);
		if (!rc && (v & mask) == want) {
			if (last)
				*last = v;
			return 0;
		}
		if (rc == -ENODEV)
			return rc;
		if (hrtimer_now_ns() >= end) {
			if (last)
				*last = v;
			return -ETIMEDOUT;
		}
		sdw_msleep(step_ms ? step_ms : 1);
	}
}

/* Interrupt status: clear `bits' before starting an operation. */
static void cs_irq_clear(struct cs42l43 *cs, int r, uint32_t bits)
{
	__atomic_and_fetch(&cs->irq_seen[r], ~bits, __ATOMIC_ACQ_REL);
	cs_wr(cs, g_irq_regs[r], bits);
}

/* Wait for any of `bits' in status register r: the latched register or
 * what the alert path already collected. */
static int cs_irq_wait(struct cs42l43 *cs, int r, uint32_t bits,
		       uint32_t timeout_ms)
{
	uint64_t end = hrtimer_now_ns() + (uint64_t)timeout_ms * 1000000ull;

	for (;;) {
		uint32_t v = 0;

		if (__atomic_load_n(&cs->irq_seen[r], __ATOMIC_ACQUIRE) & bits) {
			__atomic_and_fetch(&cs->irq_seen[r], ~bits,
					   __ATOMIC_ACQ_REL);
			return 0;
		}
		if (!cs_rd(cs, g_irq_regs[r], &v) && (v & bits)) {
			cs_wr(cs, g_irq_regs[r], v & bits);
			return 0;
		}
		if (!cs->s->link->up || cs->s->status == SDW_SLAVE_UNATTACHED)
			return -ENODEV;
		if (hrtimer_now_ns() >= end)
			return -ETIMEDOUT;
		sdw_msleep(2);
	}
}

/* Writes behind the test-key unlock (0x4000 = 0x55, 0xAA ... 0) */
static void cs_write_seq(struct cs42l43 *cs, const uint32_t (*seq)[2], int n)
{
	for (int i = 0; i < n; i++)
		cs_wr(cs, seq[i][0], seq[i][1]);
}

/* ==========================================================================
 * Boot: microcontroller and soft reset
 * ========================================================================== */

/* After (re)attach: SoundWire reads need the divided oscillator. */
static int cs_post_attach(struct cs42l43 *cs)
{
	sdw_write(cs->s, CS_GEN_INT_MASK_1, CS_INT_STAT_GEN1);
	return cs_wr(cs, CS_OSC_DIV_SEL, CS_OSC_DIV2_EN);
}

/* Soft reset: the device detaches and comes back as device 0. */
static int cs_soft_reset(struct cs42l43 *cs)
{
	int rc;

	cs->pll_on = 0;
	cs->spk_powered = cs->hp_powered = 0;
	/* the write itself may not be acknowledged: the device goes */
	cs_wr(cs, CS_SFT_RESET, CS_SFT_RESET_VAL);
	sdw_msleep(20);
	sdw_wait_detached(cs->s, 100);
	rc = sdw_wait_attached(cs->s, 5000);
	if (rc) {
		sdw_info("%s: did not come back after reset (%d)\n",
			 cs->s->name, rc);
		return rc;
	}
	return cs_post_attach(cs);
}

static int cs_mcu_stage_2_3(struct cs42l43 *cs, int shadow)
{
	uint32_t need = CS_NEED_CONFIGS, boot = CS_BOOT_STATUS, v = 0;
	int rc;

	if (cs->variant_b) {
		need = CS_B_NEED_CONFIGS;
		boot = CS_B_BOOT_STATUS;
	} else if (shadow) {
		need = CS_FW_SH_BOOT_CFG_NEED_CONFIGS;
	}
	cs_wr(cs, need, 0);
	rc = cs_poll(cs, boot, 0xffffffffu, CS_STAGE3, 20, 5, &v);
	if (rc) {
		sdw_info("%s: microcontroller stuck in stage %u\n", cs->s->name,
			 v);
		return rc;
	}
	return -EAGAIN;
}

static int cs_mcu_stage_3_2(struct cs42l43 *cs)
{
	cs_wr(cs, CS_FW_MISSION_NEED_CONFIGS, CS_FW_PATCH_NEED_CFG);
	cs_wr(cs, CS_FW_MISSION_HAVE_CONFIGS, 0);
	if (cs_soft_reset(cs))
		return -ETIMEDOUT;
	return -EAGAIN;
}

/* Stop the microcontroller so the host owns the registers. */
static int cs_mcu_disable(struct cs42l43 *cs)
{
	uint32_t cfg = CS_FW_MISSION_MM_MCU_CFG_REG;
	uint32_t sel = CS_FW_MISSION_MM_CTRL_SELECTION;
	uint32_t v = 0;
	int rc;

	if (cs->variant_b) {
		cfg = CS_B_FW_MISSION_MM_MCU_CFG_REG;
		sel = CS_B_FW_MISSION_MM_CTRL_SELECTION;
	}
	cs_wr(cs, cfg, CS_FW_MM_MCU_CFG_DISABLE);
	cs_wr(cs, sel, CS_FW_MM_CTRL_MCU_SEL);
	cs_wr(cs, CS_MCU_SW_INTERRUPT, CS_CONTROL_IND);
	cs_wr(cs, CS_MCU_SW_INTERRUPT, 0);
	rc = cs_poll(cs, CS_SOFT_INT_SHADOW, CS_CONTROL_APPLIED,
		     CS_CONTROL_APPLIED, 20, 5, &v);
	if (rc) {
		sdw_info("%s: microcontroller did not stop (0x%x)\n",
			 cs->s->name, v);
		return rc;
	}
	if (cs_soft_reset(cs))
		return -ETIMEDOUT;
	return -EAGAIN;
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

/* Load the microcontroller patch from /lib/firmware/cs42l43.bin. */
static int cs_mcu_patch(struct cs42l43 *cs)
{
	void *data = NULL;
	size_t len = 0;
	const uint8_t *fw;
	uint32_t load, v = 0;
	int rc;

	if (!sdw_can_sleep())
		return -EAGAIN;
	rc = firmware_request("cs42l43.bin", &data, &len);
	if (rc) {
		sdw_info("%s: firmware cs42l43.bin not available (%d)\n",
			 cs->s->name, rc);
		return rc;
	}
	fw = data;
	if (len < 32 || len > CS_FW_MAX_BYTES || (len & 3)) {
		sdw_info("%s: cs42l43.bin has a bad size (%u)\n", cs->s->name,
			 (uint32_t)len);
		firmware_release(data);
		return -EINVAL;
	}
	if ((fw[0] | (fw[1] << 8)) != CS_MCU_UPDATE_FORMAT) {
		sdw_info("%s: cs42l43.bin format %u not supported\n",
			 cs->s->name, (unsigned)(fw[0] | (fw[1] << 8)));
		firmware_release(data);
		return -EINVAL;
	}
	load = le32(fw + 28);
	if (load >= 0x00020000u || load + len > 0x00020000u) {
		sdw_info("%s: patch load address 0x%x out of range\n",
			 cs->s->name, load);
		firmware_release(data);
		return -EINVAL;
	}
	cs_wr(cs, cs->variant_b ? CS_B_PATCH_START_ADDR : CS_PATCH_START_ADDR,
	      load);
	rc = sdw_nwrite(cs->s, load + CS_MCU_UPDATE_OFFSET, len, fw);
	firmware_release(data);
	if (rc) {
		sdw_info("%s: patch download failed (%d)\n", cs->s->name, rc);
		return rc;
	}
	cs_wr(cs, CS_MCU_SW_INTERRUPT, CS_PATCH_IND);
	cs_wr(cs, CS_MCU_SW_INTERRUPT, 0);
	rc = cs_poll(cs, CS_SOFT_INT_SHADOW, CS_PATCH_APPLIED, CS_PATCH_APPLIED,
		     500, 10, &v);
	if (rc) {
		sdw_info("%s: patch not applied (0x%x)\n", cs->s->name, v);
		return rc;
	}
	sdw_info("%s: microcontroller patched\n", cs->s->name);
	return -EAGAIN;
}

/* One step; 0 when the device is in its final state, -EAGAIN when
 * another step is needed. */
static int cs_mcu_step(struct cs42l43 *cs)
{
	uint32_t boot_reg = cs->variant_b ? CS_B_BOOT_STATUS : CS_BOOT_STATUS;
	uint32_t rev_reg = cs->variant_b ? CS_B_MCU_SW_REV : CS_MCU_SW_REV;
	uint32_t boot = 0, rev = 0, secure = 0, mcu, bios, dummy;
	int patched, shadow;

	cs_rd(cs, CS_SOFT_INT, &dummy); /* clears stale bits */
	if (cs_rd(cs, boot_reg, &boot) || cs_rd(cs, rev_reg, &rev))
		return -EIO;
	bios = (((rev & 0x000F0000u) << 12) | ((rev & 0x00F00000u) << 4) |
		((rev & 0xFF000000u) >> 8)) >> 16;
	mcu = ((rev & 0xFu) << 12) | ((rev & 0xF0u) << 4) |
	      ((rev & 0xFF00u) >> 8);
	cs->mcu_rev = mcu;
	cs->bios_rev = bios;
	patched = mcu != CS_MCU_ROM_REV || bios != CS_MCU_ROM_BIOS_REV;
	shadow = mcu >= CS_MCU_SHADOW_REGS_REV ||
		 bios >= CS_BIOS_SHADOW_REGS_REV;
	if (cs_rd(cs, CS_BOOT_CONTROL, &secure))
		return -EIO;
	cs->hw_lock = (secure & CS_LOCK_HW_STS) ? 1 : 0;
	if (!patched && cs->hw_lock) {
		sdw_info("%s: unpatched secure device\n", cs->s->name);
		return -EPERM;
	}
	switch (boot) {
	case CS_STAGE2:
		if (!patched)
			return cs_mcu_patch(cs);
		return cs_mcu_stage_2_3(cs, shadow);
	case CS_STAGE3:
		if (patched) {
			if (mcu < CS_MCU_SUPPORTED_REV &&
			    bios < CS_MCU_SUPPORTED_BIOS_REV) {
				sdw_info("%s: firmware 0x%x/0x%x too old\n",
					 cs->s->name, mcu, bios);
				return -EINVAL;
			}
			return cs_mcu_disable(cs);
		}
		return cs_mcu_stage_3_2(cs);
	case CS_STAGE4:
		return 0;
	default:
		sdw_info("%s: unknown boot status %u\n", cs->s->name, boot);
		return -EINVAL;
	}
}

static int cs_mcu_update(struct cs42l43 *cs)
{
	for (int i = 0; i < 5; i++) {
		int rc = cs_mcu_step(cs);
		if (rc != -EAGAIN)
			return rc;
		if (cs->s->status == SDW_SLAVE_UNATTACHED) {
			rc = sdw_wait_attached(cs->s, 5000);
			if (rc)
				return rc;
			cs_post_attach(cs);
		}
	}
	sdw_info("%s: boot did not settle\n", cs->s->name);
	return -ETIMEDOUT;
}

/* The register patch every revision needs after boot. */
static void cs_register_patch(struct cs42l43 *cs)
{
	static const uint32_t seq[][2] = {
		{ 0x4000, 0x00000055 },
		{ 0x4000, 0x000000AA },
		{ 0x10084, 0x00000000 },
		{ 0x1741C, 0x00CD2000 },
		{ 0x1718C, 0x00000003 },
		{ 0x4000, 0x00000000 },
		{ CS_CCM_BLK_CLK_CONTROL, 0x00000002 },
		{ CS_HPPATHVOL, 0x011B011B },
		{ CS_OSC_DIV_SEL, 0x00000001 },
		{ CS_DACCNFG2, 0x00000005 },
		{ CS_MIC_DETECT_CONTROL_ANDROID, 0x80790079 },
		{ CS_RELID, 0x0000000F },
	};
	cs_write_seq(cs, seq, (int)(sizeof(seq) / sizeof(seq[0])));
}

/* ==========================================================================
 * PLL (from the SoundWire bus clock)
 * ========================================================================== */

struct cs_pll_cfg {
	uint32_t freq, div, mode, cal;
};

static const struct cs_pll_cfg g_pll_cfgs[] = {
	{ 2400000, 0x50000000, 0x1, 0xA4 },
	{ 3000000, 0x40000000, 0x1, 0x83 },
	{ 3072000, 0x40000000, 0x3, 0x80 },
};

static int fls32(uint32_t v)
{
	return v ? 32 - __builtin_clz(v) : 0;
}

static int cs_pll_enable(struct cs42l43 *cs)
{
	struct sdw_link *l = cs->s->link;
	uint32_t freq = l->params.curr_dr_freq / 2;
	const struct cs_pll_cfg *cfg = NULL;
	int div;

	if (cs->pll_on)
		return 0;
	div = fls32(freq) - fls32(g_pll_cfgs[2].freq);
	if (div < 0 || div > 3) {
		sdw_info("%s: no PLL set-up for a %u Hz bus\n", cs->s->name,
			 freq);
		return -EINVAL;
	}
	freq >>= div;
	for (unsigned i = 0; i < sizeof(g_pll_cfgs) / sizeof(g_pll_cfgs[0]);
	     i++)
		if (g_pll_cfgs[i].freq == freq)
			cfg = &g_pll_cfgs[i];
	if (!cfg) {
		sdw_info("%s: no PLL set-up for %u Hz (div %d)\n", cs->s->name,
			 freq, div);
		return -EINVAL;
	}
	cs_upd(cs, CS_PLL_CONTROL, CS_PLL_REFCLK_DIV_MASK | CS_PLL_REFCLK_SRC_MASK,
	       ((uint32_t)div << 1) | CS_PLL_SRC_SDW);
	cs_wr(cs, CS_FDIV_FRAC, cfg->div);
	cs_upd(cs, CS_CTRL_REG, CS_PLL_MODE_BYPASS_500 | CS_PLL_MODE_BYPASS_1029,
	       cfg->mode << 1);
	cs_upd(cs, CS_CAL_RATIO, CS_PLL_CAL_RATIO, cfg->cal);
	cs_upd(cs, CS_PLL_CONTROL, CS_PLL_REFCLK_EN, CS_PLL_REFCLK_EN);
	cs_irq_clear(cs, CS_IRQ_PLL, CS_PLL_READY | CS_PLL_LOST_LOCK);
	cs_upd(cs, CS_CTRL_REG, CS_PLL_EN, CS_PLL_EN);
	if (cs_irq_wait(cs, CS_IRQ_PLL, CS_PLL_READY, CS_PLL_TIMEOUT_MS)) {
		cs_upd(cs, CS_CTRL_REG, CS_PLL_EN, 0);
		cs_upd(cs, CS_PLL_CONTROL, CS_PLL_REFCLK_EN, 0);
		sdw_info("%s: PLL did not lock\n", cs->s->name);
		return -ETIMEDOUT;
	}
	/* switch the system clock over; SoundWire reads now use the PLL */
	cs_wr(cs, CS_OSC_DIV_SEL, 0);
	cs_upd(cs, CS_MCLK_SRC_SEL, CS_OSC_PLL_MCLK_SEL, CS_OSC_PLL_MCLK_SEL);
	snd_udelay(5);
	cs->pll_on = 1;
	cs->pll_freq = l->params.curr_dr_freq / 2;
	return 0;
}

static void cs_pll_disable(struct cs42l43 *cs)
{
	if (!cs->pll_on)
		return;
	cs_upd(cs, CS_MCLK_SRC_SEL, CS_OSC_PLL_MCLK_SEL, 0);
	snd_udelay(5);
	cs_wr(cs, CS_OSC_DIV_SEL, CS_OSC_DIV2_EN);
	cs_upd(cs, CS_CTRL_REG, CS_PLL_EN, 0);
	cs_upd(cs, CS_PLL_CONTROL, CS_PLL_REFCLK_EN, 0);
	cs->pll_on = 0;
}

static int cs_rate_code(uint32_t rate)
{
	switch (rate) {
	case 8000:
		return 0x11;
	case 16000:
		return 0x12;
	case 24000:
		return 0x02;
	case 32000:
		return 0x13;
	case 44100:
		return 0x0B;
	case 48000:
		return 0x03;
	case 96000:
		return 0x04;
	case 192000:
		return 0x05;
	default:
		return -EINVAL;
	}
}

/* ==========================================================================
 * Outputs: routing, amplifiers, volume
 * ========================================================================== */

static uint32_t spk_reg_of(uint32_t vol)
{
	if (vol > 100)
		vol = 100;
	return vol * CS_SPK_VOL_0DB / 100;
}

/* headphone: 0..100 -> -60 dB .. 0 dB in half dB, 9-bit two's complement */
static uint32_t hp_reg_of(uint32_t vol)
{
	int half;

	if (!vol)
		return CS_HP_VOL_MUTE;
	if (vol > 100)
		vol = 100;
	half = -120 + (int)(vol * 120 / 100);
	return (uint32_t)half & 0x1ffu;
}

static void cs_spk_volume_apply(struct cs42l43 *cs)
{
	int mute = cs->spk_mute || cs->master_mute || !cs->spk_powered;
	uint32_t ml = cs->spk_vol[0] * cs->master_vol / 100;
	uint32_t mr = cs->spk_vol[1] * cs->master_vol / 100;
	uint32_t l = spk_reg_of(ml), r = spk_reg_of(mr);

	if (l > CS_SPK_VOL_0DB)
		l = CS_SPK_VOL_0DB;
	if (r > CS_SPK_VOL_0DB)
		r = CS_SPK_VOL_0DB;
	cs_upd(cs, CS_INTP_VOLUME_CTRL1, CS_AMP_VOL | CS_AMP_MUTE,
	       l | (mute ? CS_AMP_MUTE : 0));
	cs_upd(cs, CS_INTP_VOLUME_CTRL2, CS_AMP_VOL | CS_AMP_MUTE,
	       r | (mute ? CS_AMP_MUTE : 0));
	/* the volume update strobe takes both channels at once */
	cs_upd(cs, CS_INTP_VOLUME_CTRL1, CS_AMP1_2_VU, CS_AMP1_2_VU);
	cs_upd(cs, CS_INTP_VOLUME_CTRL1, CS_AMP1_2_VU, 0);
}

static void cs_hp_volume_apply(struct cs42l43 *cs)
{
	int mute = cs->hp_mute || cs->master_mute;
	uint32_t l = mute ? CS_HP_VOL_MUTE :
			    hp_reg_of(cs->hp_vol[0] * cs->master_vol / 100);
	uint32_t r = mute ? CS_HP_VOL_MUTE :
			    hp_reg_of(cs->hp_vol[1] * cs->master_vol / 100);

	cs_upd(cs, CS_HPPATHVOL, CS_AMP3_PATH_VOL | CS_AMP4_PATH_VOL,
	       l | (r << 16));
}

/* Mixer: speaker and headphone from their own port when it streams,
 * otherwise from whichever playback port does. */
static void cs_mixer_route(struct cs42l43 *cs)
{
	uint32_t hp_l = CS_SRC_NONE, hp_r = CS_SRC_NONE;
	uint32_t spk_l = CS_SRC_NONE, spk_r = CS_SRC_NONE;
	uint32_t p = cs->play_ports;

	if (p & (1u << CS_DP_HP)) {
		hp_l = CS_SRC_DP5RX1;
		hp_r = CS_SRC_DP5RX2;
	} else if (p & (1u << CS_DP_SPK)) {
		hp_l = CS_SRC_DP6RX1;
		hp_r = CS_SRC_DP6RX2;
	} else if (p & (1u << CS_DP_SPARE)) {
		hp_l = CS_SRC_DP7RX1;
		hp_r = CS_SRC_DP7RX2;
	}
	if (p & (1u << CS_DP_SPK)) {
		spk_l = CS_SRC_DP6RX1;
		spk_r = CS_SRC_DP6RX2;
	} else if (p & (1u << CS_DP_HP)) {
		spk_l = CS_SRC_DP5RX1;
		spk_r = CS_SRC_DP5RX2;
	} else if (p & (1u << CS_DP_SPARE)) {
		spk_l = CS_SRC_DP7RX1;
		spk_r = CS_SRC_DP7RX2;
	}
	cs_upd(cs, CS_AMP1MIX_INPUT1, CS_MIXER_SRC_MASK | CS_MIXER_VOL_MASK,
	       spk_l | (CS_MIXER_VOL_0DB << 17));
	cs_upd(cs, CS_AMP2MIX_INPUT1, CS_MIXER_SRC_MASK | CS_MIXER_VOL_MASK,
	       spk_r | (CS_MIXER_VOL_0DB << 17));
	cs_upd(cs, CS_AMP3MIX_INPUT1, CS_MIXER_SRC_MASK | CS_MIXER_VOL_MASK,
	       hp_l | (CS_MIXER_VOL_0DB << 17));
	cs_upd(cs, CS_AMP4MIX_INPUT1, CS_MIXER_SRC_MASK | CS_MIXER_VOL_MASK,
	       hp_r | (CS_MIXER_VOL_0DB << 17));
}

static void cs_spk_power(struct cs42l43 *cs, int on)
{
	if (on == cs->spk_powered)
		return;
	if (on) {
		cs_irq_clear(cs, CS_IRQ_AMP,
			     CS_AMP1_STARTUP_DONE | CS_AMP2_STARTUP_DONE);
		cs_upd(cs, CS_BLOCK_EN10, CS_AMP1_EN | CS_AMP2_EN,
		       CS_AMP1_EN | CS_AMP2_EN);
		if (cs_irq_wait(cs, CS_IRQ_AMP,
				CS_AMP1_STARTUP_DONE | CS_AMP2_STARTUP_DONE,
				CS_SPK_TIMEOUT_MS))
			sdw_info("%s: speaker start-up not confirmed\n",
				 cs->s->name);
		cs->spk_powered = 1;
		cs_spk_volume_apply(cs); /* unmutes */
	} else {
		cs->spk_powered = 0;
		cs_spk_volume_apply(cs); /* mutes first */
		cs_irq_clear(cs, CS_IRQ_AMP,
			     CS_AMP1_SHUTDOWN_DONE | CS_AMP2_SHUTDOWN_DONE);
		cs_upd(cs, CS_BLOCK_EN10, CS_AMP1_EN | CS_AMP2_EN, 0);
		cs_irq_wait(cs, CS_IRQ_AMP,
			    CS_AMP1_SHUTDOWN_DONE | CS_AMP2_SHUTDOWN_DONE,
			    CS_SPK_TIMEOUT_MS);
	}
}

/* The headphone amplifier ramps its own volume: start it muted, open the
 * path volume after start-up, close it before shutdown - no pops. */
static void cs_hp_power(struct cs42l43 *cs, int on)
{
	if (on == cs->hp_powered)
		return;
	if (on) {
		cs_upd(cs, CS_HPPATHVOL, CS_AMP3_PATH_VOL | CS_AMP4_PATH_VOL,
		       CS_HP_VOL_MUTE | (CS_HP_VOL_MUTE << 16));
		cs_irq_clear(cs, CS_IRQ_MSM, CS_HP_STARTUP_DONE);
		cs_upd(cs, CS_BLOCK_EN8, CS_HP_EN, CS_HP_EN);
		if (cs_irq_wait(cs, CS_IRQ_MSM, CS_HP_STARTUP_DONE,
				CS_HP_TIMEOUT_MS))
			sdw_info("%s: headphone start-up not confirmed\n",
				 cs->s->name);
		cs->hp_powered = 1;
		cs_hp_volume_apply(cs);
	} else {
		cs_upd(cs, CS_HPPATHVOL, CS_AMP3_PATH_VOL | CS_AMP4_PATH_VOL,
		       CS_HP_VOL_MUTE | (CS_HP_VOL_MUTE << 16));
		sdw_msleep(10);
		cs_irq_clear(cs, CS_IRQ_MSM, CS_HP_SHUTDOWN_DONE);
		cs_upd(cs, CS_BLOCK_EN8, CS_HP_EN, 0);
		cs_irq_wait(cs, CS_IRQ_MSM, CS_HP_SHUTDOWN_DONE,
			    CS_HP_TIMEOUT_MS);
		cs->hp_powered = 0;
	}
}

/* Bring the amplifiers to what the routing and the streams want.  Lock
 * held, process context. */
static void cs_apply_power(struct cs42l43 *cs)
{
	int playing = cs->play_ports != 0 && cs->pll_on;
	int want_spk = playing && cs->spk_on;
	int want_hp = playing && cs->hp_on && cs->jack_present;

	if (cs->detecting)
		want_hp = 0;
	/* switch off first, then on: never both draw at once needlessly */
	if (!want_spk)
		cs_spk_power(cs, 0);
	if (!want_hp)
		cs_hp_power(cs, 0);
	if (want_spk)
		cs_spk_power(cs, 1);
	if (want_hp)
		cs_hp_power(cs, 1);
}

/* ==========================================================================
 * Capture
 * ========================================================================== */

static void cs_decim_apply(struct cs42l43 *cs)
{
	uint32_t reg = cs->variant_b ? CS_B_DECIM_VOL_CTRL_CH1_CH2 :
				       CS_DECIM_VOL_CTRL_CH1_CH2;
	uint32_t upd = cs->variant_b ? CS_B_DECIM_VOL_CTRL_UPDATE :
				       CS_DECIM_VOL_CTRL_UPDATE;
	uint32_t vol = cs->cap_vol > 100 ? 100 : cs->cap_vol;
	uint32_t code = vol * 0xBF / 100; /* -64 dB .. +31.5 dB */
	int mute = cs->cap_mute || !vol;

	/* pair 1/2 (A: ADC/PDM1, B: ADCs) and pair 3/4 (B: PDM1) */
	for (int pair = 0; pair < 2; pair++) {
		uint32_t r = reg + 4u * (uint32_t)pair;
		uint32_t v = ((code << 6) & CS_DECIM_LO_VOL) |
			     ((code << 22) & CS_DECIM_HI_VOL);
		if (mute)
			v |= CS_DECIM_LO_MUTE | CS_DECIM_HI_MUTE;
		cs_upd(cs, r,
		       CS_DECIM_LO_VOL | CS_DECIM_HI_VOL | CS_DECIM_LO_MUTE |
			       CS_DECIM_HI_MUTE,
		       v);
	}
	cs_wr(cs, upd, 0xf);
	cs_wr(cs, upd, 0);
}

static void cs_capture_route(struct cs42l43 *cs)
{
	uint32_t en = 0;
	uint32_t d1 = CS_SRC_DECIM1, d2 = CS_SRC_DECIM2;

	if (cs->cap_ports & (1u << CS_DP_HS_MIC)) {
		/* headset microphone: ADC1, mono on both channels */
		en |= CS_ADC1_EN;
		cs_upd(cs, CS_SWIRE_DP1_CH1_INPUT, CS_MIXER_SRC_MASK, d1);
		cs_upd(cs, CS_SWIRE_DP1_CH2_INPUT, CS_MIXER_SRC_MASK, d1);
	}
	if (cs->cap_ports & (1u << CS_DP_DMIC)) {
		/* digital microphones on PDM1: decimators 1/2 on the A
		 * part (shared with ADC1), 3/4 on the B part */
		if (cs->variant_b) {
			d1 = CS_SRC_DECIM3;
			d2 = CS_SRC_DECIM4;
		} else if (en & CS_ADC1_EN) {
			/* both at once on an A part: the headset wins */
			d1 = d2 = CS_SRC_DECIM1;
		}
		if (d1 != CS_SRC_DECIM1 || !(en & CS_ADC1_EN))
			en |= CS_PDM1_DIN_L_EN | CS_PDM1_DIN_R_EN;
		cs_upd(cs, CS_SWIRE_DP2_CH1_INPUT, CS_MIXER_SRC_MASK, d1);
		cs_upd(cs, CS_SWIRE_DP2_CH2_INPUT, CS_MIXER_SRC_MASK, d2);
	}
	cs_upd(cs, CS_BLOCK_EN3,
	       CS_ADC1_EN | CS_PDM1_DIN_L_EN | CS_PDM1_DIN_R_EN, en);
	cs_decim_apply(cs);
}

/* ==========================================================================
 * Jack detection
 * ========================================================================== */

static int cs_jack_tip(struct cs42l43 *cs, uint32_t *raw)
{
	uint32_t sts = 0;

	if (cs_rd(cs, CS_TIP_RING_SENSE_STATUS, &sts))
		return -EIO;
	if (raw)
		*raw = sts;
	return ((sts >> CS_TIPSENSE_PLUG_DB_STS_SHIFT) & 3) == 3;
}

static void cs_jack_config(struct cs42l43 *cs)
{
	/* tip sense: inverted by design, 500 ms debounce both ways */
	uint32_t tip = CS_TIPSENSE_INV | (3u << 3) | 3u;
	/* automatic type detection, 50 ms per step, 170 ms bias ramp */
	uint32_t hs2 = (2u << 15) | (4u << 8) | (3u << 26);
	uint32_t autoctl = (3u << 30) | CS_S0_AUTO_ADCMUTE_DISABLE | 0u;

	cs_upd(cs, CS_TIPSENSE_DEB_CTRL,
	       CS_TIPSENSE_INV | CS_TIPSENSE_FALL_DB | CS_TIPSENSE_RISE_DB, tip);
	cs_upd(cs, CS_HS2, CS_HSBIAS_RAMP | CS_HSDET_MODE | CS_AUTO_HSDET_TIME,
	       hs2);
	cs_upd(cs, CS_HS_BIAS_SENSE_AUTOCTL,
	       CS_JACKDET_MODE | CS_S0_AUTO_ADCMUTE_DISABLE |
		       CS_HSBIAS_SENSE_TRIP,
	       autoctl);
	cs_upd(cs, CS_PDNCNTL, CS_RING_SENSE_EN, 0);
}

static void cs_start_hs_bias(struct cs42l43 *cs, int type_detect)
{
	cs_upd(cs, CS_HS2, CS_HS_CLAMP_DISABLE, CS_HS_CLAMP_DISABLE);
	if (!type_detect)
		cs_upd(cs, CS_HS_BIAS_SENSE_AUTOCTL,
		       CS_HSBIAS_SENSE_EN | CS_AUTO_HSBIAS_CLAMP_EN,
		       CS_HSBIAS_SENSE_EN | CS_AUTO_HSBIAS_CLAMP_EN);
	cs_upd(cs, CS_MIC_DETECT_CONTROL_1, CS_HSBIAS_MODE, 0x3u << 1);
	sdw_msleep(170);
}

static void cs_stop_hs_bias(struct cs42l43 *cs)
{
	cs_upd(cs, CS_MIC_DETECT_CONTROL_1, CS_HSBIAS_MODE, 0x1u << 1);
	cs_upd(cs, CS_HS2, CS_HS_CLAMP_DISABLE, 0);
	cs_upd(cs, CS_HS_BIAS_SENSE_AUTOCTL,
	       CS_HSBIAS_SENSE_EN | CS_AUTO_HSBIAS_CLAMP_EN, 0);
}

/* Measure the load on the headphone outputs. */
static int cs_load_detect(struct cs42l43 *cs, int mic)
{
	static const uint32_t pole3[][2] = {
		{ 0x4000, 0x55 },	  { 0x4000, 0xAA },
		{ 0x17420, 0x8500F300 }, { 0x17424, 0x36003E00 },
		{ 0x4000, 0x00 },
	};
	static const uint32_t pole4[][2] = {
		{ 0x4000, 0x55 },	  { 0x4000, 0xAA },
		{ 0x17420, 0x7800E600 }, { 0x17424, 0x36003800 },
		{ 0x4000, 0x00 },
	};
	int was_on = cs->hp_powered;
	uint32_t res = 0;
	int rc, kind;

	if (was_on)
		cs_hp_power(cs, 0);
	if (mic)
		cs_write_seq(cs, pole4, 5);
	else
		cs_write_seq(cs, pole3, 5);
	cs_upd(cs, CS_BLOCK_EN3, CS_ADC1_EN | CS_ADC2_EN, 0);
	cs_upd(cs, CS_DACCNFG2, CS_HP_HPF_EN, 0);
	cs_upd(cs, CS_MIC_DETECT_CONTROL_1, CS_HSBIAS_MODE, 0);
	cs_upd(cs, CS_HP_CTRL, CS_ADPTPWR_MODE, 0x4);
	cs_upd(cs, CS_PGAVOL, CS_HP_DIG_VOL_RAMP | CS_HP_ANA_VOL_RAMP, 0x6);
	cs_upd(cs, CS_DACCNFG1, CS_HP_MSTR_VOL_CTRL_EN, 0);
	cs_upd(cs, CS_HS2, CS_HS_CLAMP_DISABLE, CS_HS_CLAMP_DISABLE);
	cs_irq_clear(cs, CS_IRQ_HPOUT, CS_HP_LOADDET_DONE);
	cs_upd(cs, CS_LOADDETENA, CS_HPLOAD_DET_EN, CS_HPLOAD_DET_EN);

	rc = cs_irq_wait(cs, CS_IRQ_HPOUT, CS_HP_LOADDET_DONE,
			 CS_LOAD_TIMEOUT_MS);

	cs_upd(cs, CS_LOADDETENA, CS_HPLOAD_DET_EN, 0);
	cs_upd(cs, CS_HS2, CS_HS_CLAMP_DISABLE, 0);
	cs_upd(cs, CS_DACCNFG1, CS_HP_MSTR_VOL_CTRL_EN, CS_HP_MSTR_VOL_CTRL_EN);
	cs_upd(cs, CS_PGAVOL, CS_HP_DIG_VOL_RAMP | CS_HP_ANA_VOL_RAMP,
	       0x4u << 8);
	cs_upd(cs, CS_HP_CTRL, CS_ADPTPWR_MODE, 0x7);
	cs_upd(cs, CS_MIC_DETECT_CONTROL_1, CS_HSBIAS_MODE, 0x1u << 1);
	cs_upd(cs, CS_DACCNFG2, CS_HP_HPF_EN, CS_HP_HPF_EN);
	if (cs->cap_ports & (1u << CS_DP_HS_MIC))
		cs_upd(cs, CS_BLOCK_EN3, CS_ADC1_EN, CS_ADC1_EN);
	if (was_on)
		cs_hp_power(cs, 1);
	if (rc)
		return rc;
	if (cs_rd(cs, CS_LOADDETRESULTS, &res))
		return -EIO;
	if (cs_jack_tip(cs, NULL) != 1)
		return -ENODEV;
	if (mic) {
		cs_start_hs_bias(cs, 0);
		return SDW_JACK_HEADPHONE | SDW_JACK_MIC;
	}
	kind = (int)(res & CS_AMP3_RES_DET);
	return kind == 3 ? SDW_JACK_LINEOUT : SDW_JACK_HEADPHONE;
}

static int cs_type_detect(struct cs42l43 *cs)
{
	uint32_t type = 0xff;
	int rc;

	cs_upd(cs, CS_STEREO_MIC_CLAMP_CTRL, CS_SMIC_HPAMP_CLAMP_DIS_FRC_VAL,
	       CS_SMIC_HPAMP_CLAMP_DIS_FRC_VAL);
	cs_start_hs_bias(cs, 1);
	cs_irq_clear(cs, CS_IRQ_MSM, CS_HSDET_DONE);
	cs_upd(cs, CS_HS2, CS_HSDET_MODE, 0x3u << 15);
	rc = cs_irq_wait(cs, CS_IRQ_MSM, CS_HSDET_DONE, 300);
	cs_upd(cs, CS_HS2, CS_HSDET_MODE, 0x2u << 15);
	cs_stop_hs_bias(cs);
	cs_upd(cs, CS_STEREO_MIC_CLAMP_CTRL, CS_SMIC_HPAMP_CLAMP_DIS_FRC_VAL, 0);
	if (rc)
		return rc;
	if (cs_rd(cs, CS_HS_STAT, &type))
		return -EIO;
	if (cs_jack_tip(cs, NULL) != 1)
		return -ENODEV;
	switch (type & CS_HSDET_TYPE_STS) {
	case 0x0: /* CTIA */
	case 0x1: /* OMTP */
	case 0x4:
		return cs_load_detect(cs, 1);
	case 0x2: /* three-pole */
		return cs_load_detect(cs, 0);
	case 0x3: /* open circuit: an extension cable */
		return SDW_JACK_MECHANICAL;
	default:
		return -EINVAL;
	}
}

static void cs_clear_jack(struct cs42l43 *cs)
{
	cs_upd(cs, CS_MIC_DETECT_CONTROL_1,
	       CS_BUTTON_DETECT_MODE | CS_MIC_LVL_DET_DISABLE,
	       CS_MIC_LVL_DET_DISABLE);
	cs_stop_hs_bias(cs);
	cs_upd(cs, CS_ADC_B_CTRL1, CS_PGA_WIDESWING, 0);
	cs_upd(cs, CS_ADC_B_CTRL2, CS_PGA_WIDESWING, 0);
	cs_upd(cs, CS_STEREO_MIC_CTRL, CS_JACK_STEREO_CONFIG, 0);
	cs_upd(cs, CS_STEREO_MIC_CLAMP_CTRL, CS_SMIC_HPAMP_CLAMP_DIS_FRC,
	       CS_SMIC_HPAMP_CLAMP_DIS_FRC);
	cs_upd(cs, CS_HS2, CS_HSDET_MODE | CS_HSDET_MANUAL_MODE, 0x2u << 15);
}

/* Look at the jack and report what changed.  Process context. */
static void cs_jack_update(struct cs42l43 *cs)
{
	uint32_t raw = 0, report = 0;
	int tip;

	if (snd_mutex_lock(&cs->lock, 0))
		return;
	if (!cs->booted || cs->s->status == SDW_SLAVE_UNATTACHED) {
		snd_mutex_unlock(&cs->lock);
		return;
	}
	tip = cs_jack_tip(cs, &raw);
	cs->last_trs = raw;
	if (tip < 0) {
		snd_mutex_unlock(&cs->lock);
		return;
	}
	if (tip && !cs->jack_present) {
		int r;
		cs->detecting = 1;
		cs_apply_power(cs);
		r = cs_type_detect(cs);
		cs->detecting = 0;
		if (r == -ENODEV) {
			/* pulled out again during the measurement */
			cs_clear_jack(cs);
			report = 0;
		} else {
			if (r < 0) {
				sdw_info("%s: jack type detection failed (%d), assuming headphones\n",
					 cs->s->name, r);
				r = SDW_JACK_HEADPHONE;
			}
			report = (uint32_t)r | SDW_JACK_MECHANICAL;
			cs->jack_present = 1;
			cs->headset = (r & SDW_JACK_MIC) ? 1 : 0;
		}
	} else if (!tip && cs->jack_present) {
		cs->jack_present = 0;
		cs->headset = 0;
		cs_clear_jack(cs);
		report = 0;
	} else {
		snd_mutex_unlock(&cs->lock);
		return;
	}
	cs->jack_report = report;
	cs_apply_power(cs);
	if (cs->cap_ports)
		cs_capture_route(cs);
	snd_mutex_unlock(&cs->lock);
	sdw_report_jack(cs->s, report,
			SDW_JACK_HEADPHONE | SDW_JACK_MIC | SDW_JACK_LINEOUT |
				SDW_JACK_MECHANICAL);
}

static void cs_jack_work(void *arg)
{
	struct cs42l43 *cs = arg;

	__atomic_store_n(&cs->jack_work_queued, 0, __ATOMIC_RELEASE);
	if (cs->s->link->ctx->stopping || !cs->s->codec)
		return;
	cs_jack_update(cs);
}

static void cs_queue_jack(struct cs42l43 *cs, uint32_t delay_ms)
{
	if (__atomic_exchange_n(&cs->jack_work_queued, 1, __ATOMIC_ACQ_REL))
		return;
	if (snd_schedule_work(cs_jack_work, cs, delay_ms))
		__atomic_store_n(&cs->jack_work_queued, 0, __ATOMIC_RELEASE);
}

/* ==========================================================================
 * Codec operations
 * ========================================================================== */

static int cs_match(const struct sof_sdw_peripheral *p)
{
	return p->mfg_id == 0x01fa && (p->part_id == 0x4243 ||
				       p->part_id == 0x2a3b);
}

static int cs_probe(struct sdw_slave *s)
{
	struct cs42l43 *cs = kcalloc(1, sizeof(*cs));

	if (!cs)
		return -ENOMEM;
	cs->s = s;
	snd_mutex_init(&cs->lock, "cs42l43");
	cs->spk_vol[0] = cs->spk_vol[1] = 100;
	cs->hp_vol[0] = cs->hp_vol[1] = 80;
	cs->cap_vol = 67; /* about 0 dB */
	cs->master_vol = 100;
	cs->spk_on = 1;
	cs->variant_b = s->p->part_id == 0x2a3b;
	s->codec = cs;
	s->caps = SDW_CAP_SPEAKER | SDW_CAP_HEADPHONE | SDW_CAP_JACK_DETECT |
		  SDW_CAP_HEADSET_MIC | SDW_CAP_DMIC | SDW_CAP_PLAYBACK |
		  SDW_CAP_CAPTURE;

	/* what the part does, whatever the firmware tables say */
	s->props.paging = 1;
	s->props.scp_int1_mask = SDW_SCP_INT1_BUS_CLASH | SDW_SCP_INT1_PARITY |
				 SDW_SCP_INT1_IMPL_DEF;
	s->props.invalid_initial_parity = 1;
	s->props.wake_capable = 1;
	s->props.source_ports = (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4);
	s->props.sink_ports = (1u << 5) | (1u << 6) | (1u << 7);
	for (int p = 1; p <= 7; p++) {
		struct sdw_dpn_props *dp = &s->props.dp[p];
		dp->valid = 1;
		dp->source = p <= 4;
		dp->type = SDW_DPN_FULL;
		dp->max_word = 24;
		dp->min_word = 8;
		dp->max_ch = (p == 1) ? 4 : 2;
		dp->simple_ch_prep_sm = 0;
		if (!dp->ch_prep_timeout_ms)
			dp->ch_prep_timeout_ms = 50;
	}
	return 0;
}

static int cs_init(struct sdw_slave *s)
{
	struct cs42l43 *cs = cs_of(s);
	uint32_t devid = 0;
	int rc;

	if (!cs)
		return -ENODEV;
	if (snd_mutex_lock(&cs->lock, 0))
		return -EINTR;
	cs->booted = 0;
	cs->pll_on = 0;
	cs->spk_powered = cs->hp_powered = 0;
	cs->jack_present = 0;
	for (int i = 0; i < CS_IRQ_NREG; i++)
		cs->irq_seen[i] = 0;

	rc = cs_post_attach(cs);
	if (rc)
		goto out;
	rc = cs_rd(cs, CS_DEVID, &devid);
	if (rc)
		goto out;
	if (devid != CS_DEVID_A && devid != CS_DEVID_B) {
		sdw_info("%s: unexpected device id 0x%06x\n", s->name, devid);
		rc = -ENODEV;
		goto out;
	}
	cs->devid = devid;
	cs->variant_b = devid == CS_DEVID_B;
	cs_rd(cs, CS_REVID, &cs->revid);
	cs_rd(cs, 0x0000B02C, &cs->otp);
	sdw_info("%s: CS42L43%s rev 0x%02x otp 0x%02x\n", s->name,
		 cs->variant_b ? "B" : "", cs->revid & 0xff, cs->otp & 0xff);

	rc = cs_mcu_update(cs);
	if (rc)
		goto out;
	cs_register_patch(cs);

	/* interrupts: everything this driver waits for or reports */
	for (int i = 0; i < CS_IRQ_NREG; i++) {
		uint32_t v = 0;
		if (!cs_rd(cs, g_irq_regs[i], &v) && v)
			cs_wr(cs, g_irq_regs[i], v);
	}
	cs_upd(cs, CS_PLL_MASK, CS_PLL_READY | CS_PLL_LOST_LOCK, 0);
	cs_upd(cs, CS_MSM_MASK,
	       CS_TIPSENSE_PLUG_DB | CS_TIPSENSE_UNPLUG_DB | CS_HSDET_DONE |
		       CS_HP_STARTUP_DONE | CS_HP_SHUTDOWN_DONE,
	       0);
	cs_upd(cs, CS_CLASS_D_AMP_MASK,
	       CS_AMP1_STARTUP_DONE | CS_AMP2_STARTUP_DONE |
		       CS_AMP1_SHUTDOWN_DONE | CS_AMP2_SHUTDOWN_DONE |
		       CS_AMP1_THERM_SHDN | CS_AMP2_THERM_SHDN |
		       CS_AMP1_SCDET | CS_AMP2_SCDET | CS_AMP1_VDDSPK_FAULT |
		       CS_AMP2_VDDSPK_FAULT,
	       0);
	cs_upd(cs, CS_HPOUT_MASK, CS_HP_LOADDET_DONE | CS_HP_ILIMIT, 0);

	/* defaults: muted speaker at the 0 dB limit, muted headphones,
	 * headphone master volume and ramps on */
	cs_upd(cs, CS_INTP_VOLUME_CTRL1, CS_AMP_VOL | CS_AMP_MUTE,
	       CS_SPK_VOL_0DB | CS_AMP_MUTE);
	cs_upd(cs, CS_INTP_VOLUME_CTRL2, CS_AMP_VOL | CS_AMP_MUTE,
	       CS_SPK_VOL_0DB | CS_AMP_MUTE);
	cs_upd(cs, CS_DACCNFG1, CS_HP_MSTR_VOL_CTRL_EN, CS_HP_MSTR_VOL_CTRL_EN);
	cs_upd(cs, CS_PGAVOL, CS_HP_DIG_VOL_RAMP, 0x4u << 8);
	cs_jack_config(cs);
	cs_mixer_route(cs);
	cs->booted = 1;
	sdw_info("%s: speaker volume limited to %u (0 dB)\n", s->name,
		 CS_SPK_VOL_0DB);
	rc = 0;
out:
	snd_mutex_unlock(&cs->lock);
	if (!rc)
		cs_queue_jack(cs, 0); /* the initial jack state */
	return rc;
}

static void cs_detached(struct sdw_slave *s)
{
	struct cs42l43 *cs = cs_of(s);

	if (!cs)
		return;
	/* registers are gone: forget the hardware state */
	cs->pll_on = 0;
	cs->spk_powered = cs->hp_powered = 0;
}

static void cs_remove(struct sdw_slave *s)
{
	struct cs42l43 *cs = cs_of(s);

	if (!cs)
		return;
	if (s->status != SDW_SLAVE_UNATTACHED && cs->booted &&
	    !snd_mutex_lock(&cs->lock, 0)) {
		cs->play_ports = cs->cap_ports = 0;
		cs_apply_power(cs);
		cs_pll_disable(cs);
		snd_mutex_unlock(&cs->lock);
	}
	/* queued jack work may still refer to the state: keep it unless
	 * none is queued */
	if (!cs->jack_work_queued) {
		s->codec = NULL;
		kfree(cs);
	}
}

static int cs_port_for(struct sdw_slave *s, int dir, int fn)
{
	struct cs42l43 *cs = cs_of(s);

	if (dir == AUDIO_DIR_PLAYBACK) {
		if (fn == SOF_SDW_FN_AMP)
			return CS_DP_SPK;
		return CS_DP_HP;
	}
	if (fn == SOF_SDW_FN_JACK)
		return CS_DP_HS_MIC;
	if (fn == SOF_SDW_FN_MIC)
		return CS_DP_DMIC;
	/* unknown: the headset microphone when one is plugged */
	if (cs && cs->headset)
		return CS_DP_HS_MIC;
	return CS_DP_DMIC;
}

static int cs_bus_config(struct sdw_slave *s, uint32_t bus_freq)
{
	struct cs42l43 *cs = cs_of(s);

	if (cs && cs->pll_on && cs->pll_freq != bus_freq) {
		sdw_info("%s: bus clock changed under a running PLL\n",
			 s->name);
		return -EBUSY;
	}
	return 0;
}

static int cs_prepare(struct sdw_slave *s, int dir, int port, uint32_t rate,
		      uint32_t ch, uint32_t bits)
{
	struct cs42l43 *cs = cs_of(s);
	int code, rc;

	(void)ch;
	(void)bits;
	if (!cs || !cs->booted)
		return -ENODEV;
	code = cs_rate_code(rate);
	if (code < 0)
		return code;
	if (snd_mutex_lock(&cs->lock, 0))
		return -EINTR;
	if ((cs->play_ports | cs->cap_ports) && cs->rate && cs->rate != rate)
		sdw_info("%s: second stream at %u Hz, first at %u Hz\n",
			 s->name, rate, cs->rate);
	cs->rate = rate;
	cs_upd(cs, CS_SAMPLE_RATE1, CS_SAMPLE_RATE_MASK, (uint32_t)code);
	rc = cs_pll_enable(cs);
	if (rc) {
		snd_mutex_unlock(&cs->lock);
		return rc;
	}
	cs_upd(cs, CS_BLOCK_EN6, CS_MIXER_EN, CS_MIXER_EN);
	if (dir == AUDIO_DIR_PLAYBACK) {
		cs->play_ports |= 1u << port;
		cs_mixer_route(cs);
		cs_apply_power(cs);
	} else {
		cs->cap_ports |= 1u << port;
		cs_capture_route(cs);
	}
	snd_mutex_unlock(&cs->lock);
	return 0;
}

static int cs_trigger(struct sdw_slave *s, int dir, int port, int on)
{
	/* amplifiers follow prepare/release; nothing time-critical here */
	(void)s;
	(void)dir;
	(void)port;
	(void)on;
	return 0;
}

static int cs_release(struct sdw_slave *s, int dir, int port)
{
	struct cs42l43 *cs = cs_of(s);

	if (!cs)
		return 0;
	if (snd_mutex_lock(&cs->lock, 0))
		return -EINTR;
	if (dir == AUDIO_DIR_PLAYBACK) {
		cs->play_ports &= ~(1u << port);
		if (cs->booted && s->status != SDW_SLAVE_UNATTACHED) {
			cs_apply_power(cs);
			cs_mixer_route(cs);
		}
	} else {
		cs->cap_ports &= ~(1u << port);
		if (cs->booted && s->status != SDW_SLAVE_UNATTACHED)
			cs_capture_route(cs);
	}
	if (!cs->play_ports && !cs->cap_ports && cs->booted &&
	    s->status != SDW_SLAVE_UNATTACHED) {
		cs_upd(cs, CS_BLOCK_EN6, CS_MIXER_EN, 0);
		cs_pll_disable(cs);
		cs->rate = 0;
	}
	snd_mutex_unlock(&cs->lock);
	return 0;
}

/* Alert path: collect and acknowledge the interrupt status, leave the
 * work to the work thread.  No codec lock (init may wait on the bus). */
static void cs_alert(struct sdw_slave *s, uint8_t int1, const uint8_t *sdca)
{
	struct cs42l43 *cs = cs_of(s);
	int jack = 0;

	(void)int1;
	(void)sdca;
	if (!cs)
		return;
	for (int i = 0; i < CS_IRQ_NREG; i++) {
		uint32_t v = 0;
		if (cs_rd(cs, g_irq_regs[i], &v) || !v || v == 0xffffffffu)
			continue;
		cs_wr(cs, g_irq_regs[i], v);
		__atomic_or_fetch(&cs->irq_seen[i], v, __ATOMIC_ACQ_REL);
		if (i == CS_IRQ_MSM &&
		    (v & (CS_TIPSENSE_PLUG_DB | CS_TIPSENSE_UNPLUG_DB |
			  CS_RINGSENSE_PLUG_DB | CS_RINGSENSE_UNPLUG_DB)))
			jack = 1;
		if (i == CS_IRQ_AMP &&
		    (v & (CS_AMP1_THERM_SHDN | CS_AMP2_THERM_SHDN |
			  CS_AMP1_SCDET | CS_AMP2_SCDET |
			  CS_AMP1_VDDSPK_FAULT | CS_AMP2_VDDSPK_FAULT))) {
			if (cs->faults++ < 8)
				sdw_info("%s: speaker amplifier fault 0x%x\n",
					 s->name, v);
		}
		if (i == CS_IRQ_HPOUT && (v & CS_HP_ILIMIT) &&
		    cs->faults++ < 8)
			sdw_info("%s: headphone current limit\n", s->name);
		if (i == CS_IRQ_PLL && (v & CS_PLL_LOST_LOCK) &&
		    cs->faults++ < 8)
			sdw_info("%s: PLL lost lock\n", s->name);
	}
	/* the SoundWire-side summary bit */
	sdw_read(s, CS_GEN_INT_STAT_1);
	sdw_write(s, CS_GEN_INT_STAT_1, CS_INT_STAT_GEN1);
	if (jack && cs->booted)
		cs_queue_jack(cs, 0);
}

static void cs_poll_op(struct sdw_slave *s)
{
	struct cs42l43 *cs = cs_of(s);
	int tip;

	if (!cs || !cs->booted || cs->detecting || cs->jack_work_queued)
		return;
	/* missed interrupts: compare the debounced tip state */
	tip = cs_jack_tip(cs, NULL);
	if (tip >= 0 && tip != cs->jack_present)
		cs_queue_jack(cs, 0);
}

static int cs_set_routing(struct sdw_slave *s, int spk, int hp)
{
	struct cs42l43 *cs = cs_of(s);

	if (!cs || !cs->booted)
		return -ENODEV;
	if (snd_mutex_lock(&cs->lock, 0))
		return -EINTR;
	cs->spk_on = spk;
	cs->hp_on = hp;
	cs_apply_power(cs);
	snd_mutex_unlock(&cs->lock);
	return 0;
}

static int cs_set_volume(struct sdw_slave *s, uint32_t ctl, uint32_t l,
			 uint32_t r)
{
	struct cs42l43 *cs = cs_of(s);

	if (!cs || !cs->booted)
		return -ENODEV;
	if (snd_mutex_lock(&cs->lock, 0))
		return -EINTR;
	switch (ctl) {
	case AUDIO_CTL_MASTER:
	case AUDIO_CTL_PCM:
		cs->master_vol = (l > r ? l : r) > 100 ? 100 : (l > r ? l : r);
		break;
	case AUDIO_CTL_SPEAKER:
		cs->spk_vol[0] = l;
		cs->spk_vol[1] = r;
		break;
	case AUDIO_CTL_HEADPHONE:
		cs->hp_vol[0] = l;
		cs->hp_vol[1] = r;
		break;
	case AUDIO_CTL_CAPTURE:
		cs->cap_vol = (l + r) / 2;
		cs_decim_apply(cs);
		snd_mutex_unlock(&cs->lock);
		return 0;
	default:
		snd_mutex_unlock(&cs->lock);
		return -ENOENT;
	}
	if (cs->spk_powered)
		cs_spk_volume_apply(cs);
	if (cs->hp_powered)
		cs_hp_volume_apply(cs);
	snd_mutex_unlock(&cs->lock);
	return 0;
}

static int cs_get_volume(struct sdw_slave *s, uint32_t ctl,
			 struct audio_volume *v)
{
	struct cs42l43 *cs = cs_of(s);

	if (!cs || !v)
		return -ENODEV;
	switch (ctl) {
	case AUDIO_CTL_SPEAKER: {
		uint32_t rl = spk_reg_of(cs->spk_vol[0]);
		uint32_t rr = spk_reg_of(cs->spk_vol[1]);
		v->left = cs->spk_vol[0];
		v->right = cs->spk_vol[1];
		v->db_left = (int32_t)rl * 50 - 6400;
		v->db_right = (int32_t)rr * 50 - 6400;
		v->db_min = -6400;
		v->db_max = 0; /* limited to 0 dB */
		v->steps = CS_SPK_VOL_0DB + 1;
		return 0;
	}
	case AUDIO_CTL_HEADPHONE:
		v->left = cs->hp_vol[0];
		v->right = cs->hp_vol[1];
		v->db_left = cs->hp_vol[0] ?
				     (-120 + (int32_t)(cs->hp_vol[0] * 120 / 100)) *
					     50 :
				     -11450;
		v->db_right = cs->hp_vol[1] ?
				      (-120 + (int32_t)(cs->hp_vol[1] * 120 / 100)) *
					      50 :
				      -11450;
		v->db_min = -6000;
		v->db_max = 0;
		v->steps = 121;
		return 0;
	case AUDIO_CTL_CAPTURE:
		v->left = v->right = cs->cap_vol;
		v->db_left = v->db_right =
			(int32_t)(cs->cap_vol * 0xBF / 100) * 50 - 6400;
		v->db_min = -6400;
		v->db_max = 3150;
		v->steps = 0xC0;
		return 0;
	default:
		return -ENOENT;
	}
}

static int cs_set_mute(struct sdw_slave *s, uint32_t ctl, uint32_t mute)
{
	struct cs42l43 *cs = cs_of(s);

	if (!cs || !cs->booted)
		return -ENODEV;
	if (snd_mutex_lock(&cs->lock, 0))
		return -EINTR;
	switch (ctl) {
	case AUDIO_CTL_MASTER:
	case AUDIO_CTL_PCM:
		cs->master_mute = mute ? 1 : 0;
		break;
	case AUDIO_CTL_SPEAKER:
		cs->spk_mute = mute ? 1 : 0;
		break;
	case AUDIO_CTL_HEADPHONE:
		cs->hp_mute = mute ? 1 : 0;
		break;
	case AUDIO_CTL_CAPTURE:
		cs->cap_mute = mute ? 1 : 0;
		cs_decim_apply(cs);
		snd_mutex_unlock(&cs->lock);
		return 0;
	default:
		snd_mutex_unlock(&cs->lock);
		return -ENOENT;
	}
	if (cs->spk_powered)
		cs_spk_volume_apply(cs);
	if (cs->hp_powered)
		cs_hp_volume_apply(cs);
	snd_mutex_unlock(&cs->lock);
	return 0;
}

static long cs_dump(struct sdw_slave *s, char *buf, size_t len)
{
	struct cs42l43 *cs = cs_of(s);
	size_t pos = 0;

	if (!cs)
		return 0;
	pos = sdw_dump_printf(buf, len, pos,
			      "    cs42l43%s: devid 0x%06x rev 0x%02x mcu 0x%04x bios 0x%04x%s %s\n",
			      cs->variant_b ? "B" : "", cs->devid,
			      cs->revid & 0xff, cs->mcu_rev, cs->bios_rev,
			      cs->hw_lock ? " locked" : "",
			      cs->booted ? "booted" : "not booted");
	pos = sdw_dump_printf(buf, len, pos,
			      "    pll %s (%u Hz bus) rate %u play ports 0x%x capture ports 0x%x\n",
			      cs->pll_on ? "on" : "off", cs->pll_freq,
			      cs->rate, cs->play_ports,
			      cs->cap_ports);
	pos = sdw_dump_printf(buf, len, pos,
			      "    speaker %s%s vol %u/%u, headphone %s%s vol %u/%u, master %u%s\n",
			      cs->spk_on ? "routed" : "off",
			      cs->spk_powered ? " powered" : "",
			      cs->spk_vol[0], cs->spk_vol[1],
			      cs->hp_on ? "routed" : "off",
			      cs->hp_powered ? " powered" : "", cs->hp_vol[0],
			      cs->hp_vol[1], cs->master_vol,
			      cs->master_mute ? " muted" : "");
	pos = sdw_dump_printf(buf, len, pos,
			      "    jack %s%s report 0x%x tip/ring 0x%x faults %u\n",
			      cs->jack_present ? "plugged" : "empty",
			      cs->headset ? " (headset)" : "", cs->jack_report,
			      cs->last_trs, cs->faults);
	return (long)pos;
}

const struct sdw_codec_ops sdw_cs42l43_ops = {
	.name = "cs42l43",
	.match = cs_match,
	.probe = cs_probe,
	.init = cs_init,
	.detached = cs_detached,
	.remove = cs_remove,
	.port_for = cs_port_for,
	.bus_config = cs_bus_config,
	.prepare = cs_prepare,
	.trigger = cs_trigger,
	.release = cs_release,
	.alert = cs_alert,
	.poll = cs_poll_op,
	.set_routing = cs_set_routing,
	.set_volume = cs_set_volume,
	.get_volume = cs_get_volume,
	.set_mute = cs_set_mute,
	.dump = cs_dump,
};
