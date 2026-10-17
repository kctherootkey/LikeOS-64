// LikeOS Ensoniq AudioPCI (ES1370 / ES1371 / ES1373 / CT5880 / EV1938)
// register definitions.
//
// The AudioPCI family is a small PCI bus-master with three DMA channels
// (two playback "DACs" and one capture "ADC"), a MIDI UART and, depending on
// the member, either an AKM AK4531 mixer/codec on a private serial bus
// (ES1370) or an AC'97 link driven through a hardware sample rate converter
// (ES1371 and everything after it).  All registers live in one 64-byte I/O
// BAR; the upper 16 bytes are a window paged by the memory page register.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_SOUND_ES1371_H
#define KERNEL_DEV_SOUND_ES1371_H

/* ---- PCI identities -------------------------------------------------------- */

#define ES_VENDOR_ENSONIQ 0x1274
#define ES_VENDOR_ECTIVA 0x1102 /* Creative's Ectiva brand */

#define ES_DEVICE_ES1370 0x5000
#define ES_DEVICE_ES1371 0x1371
#define ES_DEVICE_CT5880 0x5880
#define ES_DEVICE_EV1938 0x8938

/* All ES1371 descendants share device 0x1371 (or 0x5880); the revision
 * register tells them apart. */
#define ES_REV_ES1373_A 0x04
#define ES_REV_ES1373_B 0x06
#define ES_REV_CT5880_A 0x07 /* CT5880 sold under device 0x1371 */
#define ES_REV_ES1373_8 0x08
#define ES_REV_ES1371_B 0x09
#define ES_REV_CT5880_C 0x02 /* device 0x5880 */
#define ES_REV_CT5880_D 0x03
#define ES_REV_CT5880_E 0x04
#define ES_REV_EV1938_A 0x00

#define ES_IO_SIZE 0x40 /* the I/O BAR decodes 64 bytes */

/* ---- 0x00: interrupt / chip select control --------------------------------- */

#define ES_REG_CONTROL 0x00

/* ES1370 meaning of the upper bits */
#define ES1370_CTRL_ADC_STOP (1u << 31)
#define ES1370_CTRL_XCTL1 (1u << 30)
#define ES1370_CTRL_PCLKDIV_SHIFT 16 /* DAC2 / ADC clock divider */
#define ES1370_CTRL_PCLKDIV_MASK (0x1fffu << 16)
#define ES1370_CTRL_PCLKDIV(d) (((uint32_t)(d) & 0x1fffu) << 16)
#define ES1370_CTRL_M_SBB (1u << 14) /* DAC clock from the MPEG port */
#define ES1370_CTRL_WTSRSEL_SHIFT 12 /* DAC1 fixed rate select */
#define ES1370_CTRL_WTSRSEL_MASK (3u << 12)
#define ES1370_CTRL_WTSRSEL(x) (((uint32_t)(x) & 3u) << 12)
#define ES1370_CTRL_DAC_SYNC (1u << 11)
#define ES1370_CTRL_M_CB (1u << 9) /* capture clock from the MPEG port */
#define ES1370_CTRL_XCTL0 (1u << 8)
#define ES1370_CTRL_CDC_EN (1u << 1) /* AK4531 serial interface on */
#define ES1370_CTRL_SERR_DIS (1u << 0)

/* ES1371 / ES1373 meaning of the upper bits */
#define ES1373_CTRL_BYPASS_P1 (1u << 31) /* DAC1 skips the SRC */
#define ES1373_CTRL_BYPASS_P2 (1u << 30) /* DAC2 skips the SRC */
#define ES1373_CTRL_BYPASS_R (1u << 29) /* ADC skips the SRC */
#define ES1373_CTRL_TEST (1u << 28) /* must stay 0 */
#define ES1373_CTRL_RECEN_B (1u << 27)
#define ES1373_CTRL_SPDIF_THRU (1u << 26)
#define ES1371_CTRL_JOY_ASEL_MASK (3u << 24)
#define ES1371_CTRL_GPIO_IN(v) (((v) >> 20) & 0x0fu)
#define ES1371_CTRL_GPIO_OUT_MASK (0x0fu << 16)
#define ES1371_CTRL_GPIO_OUT(x) (((uint32_t)(x) & 0x0fu) << 16)
#define ES1371_CTRL_SYNC_RES (1u << 14) /* AC'97 warm reset */
#define ES1371_CTRL_ADC_STOP (1u << 13)
#define ES1371_CTRL_PWR_INTRM (1u << 12)
#define ES1371_CTRL_M_CB (1u << 11)
#define ES1371_CTRL_PDLEV_MASK (3u << 8)
#define ES1371_CTRL_XTALCKDIS (1u << 1)
#define ES1371_CTRL_PCICLKDIS (1u << 0)

/* shared low bits */
#define ES_CTRL_MSFMTSEL (1u << 15)
#define ES_CTRL_CCB_INTRM (1u << 10)
#define ES_CTRL_BREQ (1u << 7)
#define ES_CTRL_DAC1_EN (1u << 6)
#define ES_CTRL_DAC2_EN (1u << 5)
#define ES_CTRL_ADC_EN (1u << 4)
#define ES_CTRL_UART_EN (1u << 3)
#define ES_CTRL_JYSTK_EN (1u << 2)

/* ---- 0x04: interrupt / chip select status ---------------------------------- */

#define ES_REG_STATUS 0x04
#define ES_STAT_INTR (1u << 31) /* some source is pending */
#define ES1371_STAT_AC97_RST (1u << 29) /* CT5880: AC'97 link reset */
#define ES1373_STAT_REAR_B27 (1u << 27)
#define ES1373_STAT_REAR_B26 (1u << 26)
#define ES1373_STAT_REAR_B24 (1u << 24)
#define ES1373_STAT_GPIO_INT_EN_MASK (0x0fu << 20)
#define ES1373_STAT_SPDIF_EN (1u << 18)
#define ES1373_STAT_SPDIF_TEST (1u << 17)
#define ES1371_STAT_TEST (1u << 16)
#define ES1370_STAT_CSTAT (1u << 10) /* AK4531 busy or write pending */
#define ES1370_STAT_CBUSY (1u << 9)
#define ES1370_STAT_CWRIP (1u << 8)
#define ES1371_STAT_SYNC_ERR (1u << 8)
#define ES1371_STAT_MPWR (1u << 5) /* power level change */
#define ES_STAT_MCCB (1u << 4) /* CCB (MPEG port) */
#define ES_STAT_UART (1u << 3)
#define ES_STAT_DAC1 (1u << 2)
#define ES_STAT_DAC2 (1u << 1)
#define ES_STAT_ADC (1u << 0)
#define ES_STAT_SOURCES                                                  \
	(ES_STAT_MCCB | ES_STAT_UART | ES_STAT_DAC1 | ES_STAT_DAC2 |     \
	 ES_STAT_ADC)

/* ---- 0x08..0x0a: MIDI UART ------------------------------------------------- */

#define ES_REG_UART_DATA 0x08
#define ES_REG_UART_STATUS 0x09 /* read */
#define ES_REG_UART_CONTROL 0x09 /* write */
#define ES_REG_UART_RES 0x0a
#define ES_UART_RXINT (1u << 7)
#define ES_UART_TXINT (1u << 2)
#define ES_UART_TXRDY (1u << 1)
#define ES_UART_RXRDY (1u << 0)

/* ---- 0x0c: memory page select for the 0x30..0x3f window --------------------- */

#define ES_REG_MEM_PAGE 0x0c
#define ES_PAGE_DAC 0x0c /* DAC1 + DAC2 frame registers */
#define ES_PAGE_ADC 0x0d /* ADC + phantom frame registers */
#define ES_PAGE_UART 0x0e
#define ES_PAGE_UART1 0x0f

/* ---- 0x10: ES1370 AK4531 write port --------------------------------------- */

#define ES1370_REG_CODEC 0x10 /* 16-bit write: (register << 8) | data */

/* ---- 0x10: ES1371 sample rate converter interface ------------------------- */

#define ES1371_REG_SMPRATE 0x10
#define ES1371_SMP_ADDR_SHIFT 25
#define ES1371_SMP_ADDR(a) (((uint32_t)(a) & 0x7fu) << 25)
#define ES1371_SMP_WE (1u << 24) /* this access writes SRC RAM */
#define ES1371_SMP_BUSY (1u << 23) /* SRC RAM access in progress */
#define ES1371_SMP_DISABLE (1u << 22) /* whole converter off */
#define ES1371_SMP_DIS_P1 (1u << 21) /* freeze DAC1 accumulator */
#define ES1371_SMP_DIS_P2 (1u << 20) /* freeze DAC2 accumulator */
#define ES1371_SMP_DIS_R1 (1u << 19) /* freeze ADC accumulator */
#define ES1371_SMP_DATA_MASK 0xffffu
/* The converter control bits a read-modify-write must carry over. */
#define ES1371_SMP_KEEP                                                     \
	(ES1371_SMP_DISABLE | ES1371_SMP_DIS_P1 | ES1371_SMP_DIS_P2 |       \
	 ES1371_SMP_DIS_R1)
/* Undocumented internal state of the converter's AC-link slot machine,
 * visible in bits 23 and 18..16.  The AC'97 register must only be written
 * while the converter sits in state 1, otherwise the access can collide
 * with the converter's own use of the link. */
#define ES1371_SMP_STATE_MASK 0x00870000u
#define ES1371_SMP_STATE_IDLE 0x00000000u
#define ES1371_SMP_STATE_SAFE 0x00010000u

/* SRC RAM layout: a 128-word RAM holding three converter channels plus
 * per-channel volumes.  Each channel occupies four words at its base. */
#define ES1371_SRC_DAC1 0x70
#define ES1371_SRC_DAC2 0x74
#define ES1371_SRC_ADC 0x78
#define ES1371_SRC_VOL_ADC 0x6c /* +0 left, +1 right */
#define ES1371_SRC_VOL_DAC1 0x7c
#define ES1371_SRC_VOL_DAC2 0x7e
#define ES1371_SRC_TRUNC_N 0x00 /* channel word 0 */
#define ES1371_SRC_INT_REGS 0x01 /* word 1: integer part in bits 15..10 */
#define ES1371_SRC_ACCUM_FRAC 0x02
#define ES1371_SRC_VFREQ_FRAC 0x03 /* word 3: 15-bit fraction */
#define ES1371_SRC_RAM_WORDS 0x80
#define ES1371_SRC_UNITY_VOL (1u << 12) /* 1.0 in the volume words */

/* ---- 0x14: ES1371 AC'97 codec access -------------------------------------- */

#define ES1371_REG_CODEC 0x14
#define ES1371_CODEC_RDY (1u << 31) /* read data valid */
#define ES1371_CODEC_WIP (1u << 30) /* an access is still running */
#define EV1938_CODEC_MAGIC (1u << 26) /* EV1938 wants this on every access */
#define ES1371_CODEC_PIRD (1u << 23) /* 1 = read, 0 = write */
#define ES1371_CODEC_ADDR(a) (((uint32_t)(a) & 0x7fu) << 16)
#define ES1371_CODEC_DATA(v) ((v) & 0xffffu)

/* ---- 0x18: ES1371 legacy (SoundBlaster trap) control ---------------------- */

#define ES1371_REG_LEGACY 0x18
#define ES1371_LEG_JFAST (1u << 31)
#define ES1371_LEG_HIB (1u << 30)
#define ES1371_LEG_FIRQ (1u << 24) /* force an interrupt (self test) */

/* ---- 0x1c: ES1373 S/PDIF channel status ------------------------------------ */

#define ES1373_REG_CHANNEL_STATUS 0x1c

/* ---- 0x20: serial interface control --------------------------------------- */

#define ES_REG_SERIAL 0x20
#define ES1371_SER_DAC_TEST (1u << 22)
#define ES_SER_P2_END_INC_MASK (7u << 19)
#define ES_SER_P2_END_INC(x) (((uint32_t)(x) & 7u) << 19)
#define ES_SER_P2_ST_INC_MASK (7u << 16)
#define ES_SER_P2_ST_INC(x) (((uint32_t)(x) & 7u) << 16)
#define ES_SER_R1_LOOP_SEL (1u << 15) /* 1 = stop at the end of the frame */
#define ES_SER_P2_LOOP_SEL (1u << 14)
#define ES_SER_P1_LOOP_SEL (1u << 13)
#define ES_SER_P2_PAUSE (1u << 12)
#define ES_SER_P1_PAUSE (1u << 11)
#define ES_SER_R1_INT_EN (1u << 10)
#define ES_SER_P2_INT_EN (1u << 9)
#define ES_SER_P1_INT_EN (1u << 8)
#define ES_SER_P1_SCT_RLD (1u << 7) /* reload DAC1 sample counter */
#define ES_SER_P2_DAC_SEN (1u << 6) /* in stop mode: replay last sample */
#define ES_SER_R1_MODE_SHIFT 4
#define ES_SER_P2_MODE_SHIFT 2
#define ES_SER_P1_MODE_SHIFT 0
#define ES_SER_MODE_MASK 3u
/* mode values */
#define ES_MODE_8_MONO 0
#define ES_MODE_8_STEREO 1
#define ES_MODE_16_MONO 2
#define ES_MODE_16_STEREO 3

/* ---- 0x24..0x2c: sample (frame) counters ----------------------------------- */

#define ES_REG_DAC1_COUNT 0x24
#define ES_REG_DAC2_COUNT 0x28
#define ES_REG_ADC_COUNT 0x2c
/* low 16 bits: frames per interrupt minus one; high 16: current count */
#define ES_COUNT_CURRENT(v) (((v) >> 16) & 0xffffu)

/* ---- 0x30..0x3c: paged frame registers ------------------------------------ */

/* page ES_PAGE_DAC */
#define ES_REG_DAC1_FRAME 0x30
#define ES_REG_DAC1_SIZE 0x34
#define ES_REG_DAC2_FRAME 0x38
#define ES_REG_DAC2_SIZE 0x3c
/* page ES_PAGE_ADC */
#define ES_REG_ADC_FRAME 0x30
#define ES_REG_ADC_SIZE 0x34
#define ES_REG_PHANTOM_FRAME 0x38
#define ES_REG_PHANTOM_COUNT 0x3c
/* frame size register: low 16 bits = longwords in the buffer minus one,
 * high 16 bits = the longword the channel is at now */
#define ES_FSIZE_CURRENT_BYTES(v) ((((v) >> 16) & 0xffffu) << 2)
#define ES_FSIZE_MAX_BYTES (0x10000u * 4u) /* 256 KiB */

/* ---- ES1370 clocks ---------------------------------------------------------- */

/* DAC2 and the ADC share one programmable clock: rate = 1411200 / (div+2) */
#define ES1370_PCLK 1411200u
#define ES1370_PCLKDIV_MIN 29u /* about 48.6 kHz */
#define ES1370_PCLKDIV_MAX 0x1fffu
/* DAC1 runs from fixed crystal-derived rates (WTSRSEL 0..3) */

/* ---- AKM AK4531 (ES1370 mixer) ---------------------------------------------- */

#define AK4531_LMASTER 0x00 /* bit7 mute, 4..0 attenuation, 2 dB steps */
#define AK4531_RMASTER 0x01
#define AK4531_LVOICE 0x02 /* bit7 mute, 4..0 gain: 0 = +12 dB, 2 dB steps */
#define AK4531_RVOICE 0x03 /* "voice" = ES1370 DAC2 */
#define AK4531_LFM 0x04 /* "FM" = ES1370 DAC1 */
#define AK4531_RFM 0x05
#define AK4531_LCD 0x06
#define AK4531_RCD 0x07
#define AK4531_LLINE 0x08
#define AK4531_RLINE 0x09
#define AK4531_LAUXA 0x0a
#define AK4531_RAUXA 0x0b
#define AK4531_MONO1 0x0c
#define AK4531_MONO2 0x0d
#define AK4531_MIC 0x0e
#define AK4531_MONO_OUT 0x0f /* bit7 mute, 2..0 attenuation, 4 dB steps */
#define AK4531_OUT_SW1 0x10 /* output mixer routing */
#define AK4531_OUT_SW2 0x11
#define AK4531_LIN_SW1 0x12 /* left ADC input mixer routing */
#define AK4531_RIN_SW1 0x13
#define AK4531_LIN_SW2 0x14
#define AK4531_RIN_SW2 0x15
#define AK4531_RESET 0x16 /* bit1 = not powered down, bit0 = not in reset */
#define AK4531_CLOCK 0x17 /* clock select */
#define AK4531_AD_IN 0x18 /* ADC input select: 0 = input mixer */
#define AK4531_MIC_GAIN 0x19 /* bit0 = +30 dB microphone amplifier */
#define AK4531_NREGS 0x1a

#define AK4531_MUTE 0x80
#define AK4531_VOL_MASK 0x1f
#define AK4531_MASTER_MAX 31 /* 0 dB .. -62 dB */
#define AK4531_GAIN_0DB 6 /* input volumes: register 6 = 0 dB */
#define AK4531_GAIN_MAX 31 /* +12 dB .. -50 dB */

/* routing bits shared by OUT_SW1 and the LIN/RIN_SW1 registers */
#define AK4531_SW1_FM_L (1u << 6)
#define AK4531_SW1_FM_R (1u << 5)
#define AK4531_SW1_LINE_L (1u << 4)
#define AK4531_SW1_LINE_R (1u << 3)
#define AK4531_SW1_CD_L (1u << 2)
#define AK4531_SW1_CD_R (1u << 1)
#define AK4531_SW1_MIC (1u << 0)
/* OUT_SW2 */
#define AK4531_SW2_AUX_L (1u << 5)
#define AK4531_SW2_AUX_R (1u << 4)
#define AK4531_SW2_VOICE_L (1u << 3)
#define AK4531_SW2_VOICE_R (1u << 2)
#define AK4531_SW2_MONO2 (1u << 1)
#define AK4531_SW2_MONO1 (1u << 0)

/* ---- AC'97 codec registers used by this driver ------------------------------ */

#define AC97_RESET 0x00
#define AC97_MASTER 0x02
#define AC97_HEADPHONE 0x04
#define AC97_MASTER_MONO 0x06
#define AC97_PC_BEEP 0x0a
#define AC97_PHONE 0x0c
#define AC97_MIC 0x0e
#define AC97_LINE 0x10
#define AC97_CD 0x12
#define AC97_VIDEO 0x14
#define AC97_AUX 0x16
#define AC97_PCM 0x18
#define AC97_REC_SEL 0x1a
#define AC97_REC_GAIN 0x1c
#define AC97_GENERAL_PURPOSE 0x20
#define AC97_POWERDOWN 0x26
#define AC97_EXTENDED_ID 0x28
#define AC97_EXTENDED_STATUS 0x2a
#define AC97_PCM_FRONT_DAC_RATE 0x2c
#define AC97_PCM_LR_ADC_RATE 0x32
#define AC97_VENDOR_ID1 0x7c
#define AC97_VENDOR_ID2 0x7e

#define AC97_MUTE 0x8000u
#define AC97_RESET_CAP_HP (1u << 4) /* AC97_RESET: headphone out exists */
#define AC97_MIC_BOOST (1u << 6) /* AC97_MIC: +20 dB */
#define AC97_PD_READY 0x000fu /* ADC, DAC, analog mixer, Vref ready */
#define AC97_EI_VRA (1u << 0)
#define AC97_EI_SPDIF (1u << 2)

#define AC97_PCM_0DB 0x08 /* PCM out gain: 0 = +12 dB, 8 = 0 dB, 1.5 dB steps */
#define AC97_PCM_MAX 0x1f

/* record select source codes */
#define AC97_REC_MIC 0
#define AC97_REC_CD 1
#define AC97_REC_VIDEO 2
#define AC97_REC_AUX 3
#define AC97_REC_LINE 4
#define AC97_REC_STEREO_MIX 5
#define AC97_REC_MONO_MIX 6
#define AC97_REC_PHONE 7

#endif /* KERNEL_DEV_SOUND_ES1371_H */
