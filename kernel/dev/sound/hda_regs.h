// LikeOS Intel High Definition Audio - controller register map.
//
// Offsets and bit fields of the HD Audio controller register window (BAR0)
// as defined by the High Definition Audio specification revision 1.0a,
// plus the Intel extended capability structures (processing pipe, multi
// link, software position based FIFO, global time sync, DMA resume) of the
// Skylake and later PCH/SoC controllers and the PCI configuration space
// registers the vendors use for traffic class and snoop control.
//
// The second half declares what the controller files (hda_controller.c,
// hda_stream.c, hda_pci_ids.c) share among themselves beyond hda.h.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_SOUND_HDA_REGS_H
#define KERNEL_DEV_SOUND_HDA_REGS_H

#include "hda.h"
#include <kernel/ke/syscall.h> /* errno values */

/* ---- global registers ----------------------------------------------------- */

#define HDA_REG_GCAP 0x00 /* 16: global capabilities */
#define HDA_GCAP_64OK (1u << 0) /* 64-bit DMA addresses */
#define HDA_GCAP_NSDO_SHIFT 1 /* serial data out signals */
#define HDA_GCAP_NSDO_MASK (3u << 1)
#define HDA_GCAP_BSS_SHIFT 3 /* bidirectional streams */
#define HDA_GCAP_BSS_MASK (0x1fu << 3)
#define HDA_GCAP_ISS_SHIFT 8 /* input streams */
#define HDA_GCAP_ISS_MASK (0xfu << 8)
#define HDA_GCAP_OSS_SHIFT 12 /* output streams */
#define HDA_GCAP_OSS_MASK (0xfu << 12)

#define HDA_REG_VMIN 0x02 /* 8 */
#define HDA_REG_VMAJ 0x03 /* 8 */
#define HDA_REG_OUTPAY 0x04 /* 16: output payload capability (words) */
#define HDA_REG_INPAY 0x06 /* 16 */

#define HDA_REG_GCTL 0x08 /* 32: global control */
#define HDA_GCTL_CRST (1u << 0) /* controller out of reset when 1 */
#define HDA_GCTL_FCNTRL (1u << 1) /* flush control */
#define HDA_GCTL_UNSOL (1u << 8) /* accept unsolicited responses */

#define HDA_REG_WAKEEN 0x0c /* 16: codec wake enables */
#define HDA_REG_STATESTS 0x0e /* 16: codec state change (W1C) */
#define HDA_STATESTS_MASK 0x7fffu

#define HDA_REG_GSTS 0x10 /* 16: global status */
#define HDA_GSTS_FSTS (1u << 1) /* flush done */

#define HDA_REG_GCAP2 0x12 /* 16 (Intel) */
#define HDA_REG_LLCH 0x14 /* 16: first extended capability (Intel) */
#define HDA_REG_OUTSTRMPAY 0x18 /* 16 */
#define HDA_REG_INSTRMPAY 0x1a /* 16 */

#define HDA_REG_INTCTL 0x20 /* 32: interrupt control */
#define HDA_INT_SIE_MASK 0x3fffffffu /* one bit per stream descriptor */
#define HDA_INT_CIE (1u << 30) /* controller interrupt enable */
#define HDA_INT_GIE (1u << 31) /* global interrupt enable */

#define HDA_REG_INTSTS 0x24 /* 32: interrupt status */
#define HDA_INT_SIS_MASK 0x3fffffffu
#define HDA_INT_CIS (1u << 30)
#define HDA_INT_GIS (1u << 31)

#define HDA_REG_WALCLK 0x30 /* 32: wall clock, 24 MHz */
#define HDA_REG_OLD_SSYNC 0x34 /* 32: stream sync on the ICH parts */
#define HDA_REG_SSYNC 0x38 /* 32: stream synchronisation */

/* command output ring buffer */
#define HDA_REG_CORBLBASE 0x40 /* 32 */
#define HDA_REG_CORBUBASE 0x44 /* 32 */
#define HDA_REG_CORBWP 0x48 /* 16: write pointer (bits 7:0) */
#define HDA_REG_CORBRP 0x4a /* 16: read pointer */
#define HDA_CORBRP_RST (1u << 15) /* read pointer reset */
#define HDA_REG_CORBCTL 0x4c /* 8 */
#define HDA_CORBCTL_CMEIE (1u << 0) /* memory error interrupt enable */
#define HDA_CORBCTL_RUN (1u << 1) /* DMA engine run */
#define HDA_REG_CORBSTS 0x4d /* 8 */
#define HDA_CORBSTS_CMEI (1u << 0) /* memory error (W1C) */
#define HDA_REG_CORBSIZE 0x4e /* 8 */

/* response input ring buffer */
#define HDA_REG_RIRBLBASE 0x50 /* 32 */
#define HDA_REG_RIRBUBASE 0x54 /* 32 */
#define HDA_REG_RIRBWP 0x58 /* 16: write pointer (bits 7:0) */
#define HDA_RIRBWP_RST (1u << 15) /* write pointer reset */
#define HDA_REG_RINTCNT 0x5a /* 16: responses per interrupt */
#define HDA_REG_RIRBCTL 0x5c /* 8 */
#define HDA_RIRBCTL_RINTCTL (1u << 0) /* response interrupt enable */
#define HDA_RIRBCTL_DMAEN (1u << 1) /* DMA engine run */
#define HDA_RIRBCTL_OIC (1u << 2) /* overrun interrupt enable */
#define HDA_REG_RIRBSTS 0x5d /* 8 */
#define HDA_RIRBSTS_RINTFL (1u << 0) /* response interrupt (W1C) */
#define HDA_RIRBSTS_RIRBOIS (1u << 2) /* overrun (W1C) */
#define HDA_REG_RIRBSIZE 0x5e /* 8 */

/* CORBSIZE / RIRBSIZE fields */
#define HDA_RINGSIZE_SZ_MASK 0x03u
#define HDA_RINGSIZE_SZ_2 0x00u /* 2 entries */
#define HDA_RINGSIZE_SZ_16 0x01u /* 16 entries */
#define HDA_RINGSIZE_SZ_256 0x02u /* 256 entries */
#define HDA_RINGSIZE_CAP_SHIFT 4
#define HDA_RINGSIZE_CAP_2 (1u << 4)
#define HDA_RINGSIZE_CAP_16 (1u << 5)
#define HDA_RINGSIZE_CAP_256 (1u << 6)

/* the RIRB entry's upper dword */
#define HDA_RIRB_EX_CODEC_MASK 0x0fu
#define HDA_RIRB_EX_UNSOL (1u << 4)

/* immediate command interface */
#define HDA_REG_IC 0x60 /* 32: immediate command */
#define HDA_REG_IR 0x64 /* 32: immediate response */
#define HDA_REG_IRS 0x68 /* 16: immediate status */
#define HDA_IRS_ICB (1u << 0) /* command busy */
#define HDA_IRS_IRV (1u << 1) /* response valid (W1C) */

/* DMA position buffer */
#define HDA_REG_DPLBASE 0x70 /* 32 */
#define HDA_REG_DPUBASE 0x74 /* 32 */
#define HDA_DPLBASE_ENABLE (1u << 0)

/* ---- stream descriptors --------------------------------------------------- */

#define HDA_REG_SD_BASE 0x80
#define HDA_REG_SD_STRIDE 0x20
#define HDA_SD_OFF(index) (HDA_REG_SD_BASE + (uint32_t)(index) * HDA_REG_SD_STRIDE)

#define HDA_SD_CTL 0x00 /* 24 bits: bytes 0..2 */
#define HDA_SD_CTL2 0x02 /* the third control byte */
#define HDA_SD_STS 0x03 /* 8 */
#define HDA_SD_LPIB 0x04 /* 32: link position in buffer */
#define HDA_SD_CBL 0x08 /* 32: cyclic buffer length */
#define HDA_SD_LVI 0x0c /* 16: last valid index */
#define HDA_SD_FIFOW 0x0e /* 16: FIFO watermark */
#define HDA_SD_FIFOS 0x10 /* 16: FIFO size */
#define HDA_SD_FMT 0x12 /* 16: stream format */
#define HDA_SD_FIFOL 0x14 /* 32: FIFO limit (Intel) */
#define HDA_SD_BDPL 0x18 /* 32: BDL pointer, low (128-byte aligned) */
#define HDA_SD_BDPU 0x1c /* 32: BDL pointer, high */

/* SDnCTL, byte 0 */
#define HDA_SD_CTL_SRST (1u << 0) /* stream reset */
#define HDA_SD_CTL_RUN (1u << 1) /* DMA run */
#define HDA_SD_CTL_IOCE (1u << 2) /* interrupt on completion enable */
#define HDA_SD_CTL_FEIE (1u << 3) /* FIFO error interrupt enable */
#define HDA_SD_CTL_DEIE (1u << 4) /* descriptor error interrupt enable */
#define HDA_SD_CTL_INT_MASK (HDA_SD_CTL_IOCE | HDA_SD_CTL_FEIE | HDA_SD_CTL_DEIE)
/* SDnCTL, byte 2 (bits 23:16 of the register) */
#define HDA_SD_CTL2_STRIPE_MASK 0x03u /* bits 17:16 */
#define HDA_SD_CTL2_TP (1u << 2) /* bit 18: traffic priority */
#define HDA_SD_CTL2_DIR (1u << 3) /* bit 19: bidirectional is output */
#define HDA_SD_CTL2_STRM_SHIFT 4 /* bits 23:20: stream tag */
#define HDA_SD_CTL2_STRM_MASK (0xfu << 4)
/* the same fields in a dword view of SDnCTL */
#define HDA_SD_CTL_DIR_DW (1u << 19)
#define HDA_SD_CTL_STRM_SHIFT_DW 20
#define HDA_SD_CTL_STRM_MASK_DW (0xfu << 20)

/* SDnSTS */
#define HDA_SD_STS_BCIS (1u << 2) /* buffer completion (W1C) */
#define HDA_SD_STS_FIFOE (1u << 3) /* FIFO error (W1C) */
#define HDA_SD_STS_DESE (1u << 4) /* descriptor error (W1C) */
#define HDA_SD_STS_FIFORDY (1u << 5) /* FIFO ready */
#define HDA_SD_STS_INT_MASK (HDA_SD_STS_BCIS | HDA_SD_STS_FIFOE | HDA_SD_STS_DESE)

/* BDL entry flags */
#define HDA_BDLE_IOC 0x1u

/* Position buffer: one 8-byte slot per stream descriptor, the first dword
 * is the position. */
#define HDA_POSBUF_STRIDE_DW 2

/* ---- Intel vendor-specific registers -------------------------------------- */

#define HDA_REG_HSW_EM4 0x100c /* display link BCLK M value */
#define HDA_REG_HSW_EM5 0x1010 /* display link BCLK N value */
#define HDA_REG_VS_EM1 0x1000
#define HDA_REG_VS_EM2 0x1030
#define HDA_REG_VS_EM4L 0x1040 /* Apollo Lake DMA fetch threshold */
#define HDA_REG_VS_LTRP 0x1048
#define HDA_REG_VS_D0I3C 0x104a
#define HDA_REG_VS_PCE 0x104b
#define HDA_EM4L_DMA_LATENCY_KEEP (0x3u << 20)

/* ---- Intel extended capabilities (Skylake and later) ---------------------- */

/* The capability header: version 31:28, id 27:16, next pointer 15:0. */
#define HDA_XCAP_VER_SHIFT 28
#define HDA_XCAP_ID_SHIFT 16
#define HDA_XCAP_ID_MASK 0xfffu
#define HDA_XCAP_NEXT_MASK 0xffffu
#define HDA_XCAP_MAX_WALK 16

#define HDA_XCAP_ID_GTS 0x1 /* global time sync */
#define HDA_XCAP_ID_ML 0x2 /* multiple links */
#define HDA_XCAP_ID_PP 0x3 /* processing pipe (DSP coupling) */
#define HDA_XCAP_ID_SPB 0x4 /* software position based FIFO */
#define HDA_XCAP_ID_DRSM 0x5 /* DMA resume */

/* processing pipe */
#define HDA_PP_PPCH 0x00
#define HDA_PP_PPCTL 0x04
#define HDA_PPCTL_PIE (1u << 31) /* processing pipe interrupt enable */
#define HDA_PPCTL_GPROCEN (1u << 30) /* decoupled (DSP) mode */
#define HDA_PP_PPSTS 0x08

/* multiple links */
#define HDA_ML_MLCH 0x00
#define HDA_ML_MLCD 0x04 /* link count - 1 in bits 3:0 */
#define HDA_ML_LINK_BASE 0x40
#define HDA_ML_LINK_STRIDE 0x40
#define HDA_ML_LCAP 0x00
#define HDA_ML_LCAP_ALT (1u << 28) /* not an HDA link (SoundWire, ...) */
#define HDA_ML_LCAP_SCF_MASK 0x3fu /* supported clock frequencies */
#define HDA_ML_LCTL 0x04
#define HDA_ML_LCTL_CPA (1u << 23) /* current power active */
#define HDA_ML_LCTL_SPA (1u << 16) /* set power active */
#define HDA_ML_LCTL_SCF_MASK 0xfu /* selected clock frequency */
#define HDA_ML_LOSIDV 0x08
#define HDA_ML_LSDIID 0x0c /* SDI identifiers present on the link */

/* software position based FIFO */
#define HDA_SPB_SPBFCH 0x00
#define HDA_SPB_SPBFCCTL 0x04 /* per-stream SPIB enable bits */
#define HDA_SPB_BASE 0x08
#define HDA_SPB_STRIDE 0x08

/* DMA resume */
#define HDA_DRSM_CTL 0x04

/* ---- PCI configuration space ------------------------------------------------ */

#define HDA_PCI_TCSEL 0x44 /* Intel: traffic class select, bits 2:0 */
#define HDA_PCI_INTEL_CGCTL 0x48 /* Skylake+: clock gating control */
#define HDA_PCI_CGCTL_MISCBDCGE (1u << 6)
#define HDA_PCI_INTEL_DEVC 0x78 /* PCH/SCH: device control */
#define HDA_PCI_DEVC_NOSNOOP (1u << 11)
#define HDA_PCI_ATI_MISC_CNTR2 0x42 /* ATI SB450+: snoop */
#define HDA_PCI_ATI_ENABLE_SNOOP 0x02u
#define HDA_PCI_NV_TRANSREG 0x4e /* NVIDIA: coherency bits */
#define HDA_PCI_NV_ENABLE_COHBITS 0x0fu
#define HDA_PCI_NV_ISTRM_COH 0x4d
#define HDA_PCI_NV_OSTRM_COH 0x4c
#define HDA_PCI_NV_ENABLE_COHBIT 0x01u
#define HDA_PCI_ULI_FIXUP 0x40 /* ULi M5461: base address fixup */
#define HDA_PCI_CAP_PM 0x01

/* ---- controller-private flags ------------------------------------------------ *
 *
 * ctrl->flags carries the HDA_F_* bits of hda.h in bits 0..24.  The top
 * bits are private to the controller files: table quirks hda.h has no
 * name for and runtime state. */
#define HDA_FX_NO_TCSEL (1u << 25) /* leave TCSEL alone */
#define HDA_FX_OLD_SSYNC (1u << 26) /* SSYNC at 0x34 (ICH6..ICH10) */
#define HDA_FX_CORBRP_SELF_CLEAR (1u << 27) /* CORBRP reset clears itself */
#define HDA_FX_POLL_RIRB (1u << 28) /* RIRB interrupt unreliable */
#define HDA_FX_SDCTL_DWORD (1u << 29) /* SDnCTL only in dword accesses */
#define HDA_FX_SNOOP_OFF (1u << 30) /* snooping does not work: no snoop */
#define HDA_FX_GONE (1u << 31) /* runtime: the device reads all-ones */

/* ---- shared between the controller files -------------------------------------- */

/* hda_pci_ids.c: the name of the card for a controller ("HDA Intel PCH"). */
const char *hda_pci_card_name(const struct hda_pci_id *id,
			      const pci_device_t *pci);
/* hda_pci_ids.c: 1 if the PCI function looks like an HD Audio controller
 * (class 0x0403, or listed in the table). */
int hda_pci_is_hda(const pci_device_t *pci);

/* hda_stream.c: lay out the stream descriptors from num_iss/oss/bss:
 * register offsets, directions and stream tags.  Called once after GCAP
 * has been read; the BDLs are allocated on first use. */
void hda_streams_init(struct hda_controller *ctrl);
/* Free every BDL (controller teardown). */
void hda_streams_fini(struct hda_controller *ctrl);
/* Stop every engine and clear its status (controller reset, teardown). */
void hda_streams_stop_all(struct hda_controller *ctrl);
/* Start or stop a set of engines together through SSYNC.  `mask' has a bit
 * per engine index. */
void hda_streams_start_sync(struct hda_controller *ctrl, uint32_t mask);
void hda_streams_stop_sync(struct hda_controller *ctrl, uint32_t mask);
/* The bytes the FIFO of a running stream holds (0 if unknown). */
uint32_t hda_stream_fifo_bytes(struct hda_stream *st);
/* Extra delay between the position reported and the link (bytes), when
 * the controller counts it (LPIB behind the position buffer). */
uint32_t hda_stream_link_delay(struct hda_stream *st);
/* The sample rate encoded in a format word (0 if invalid). */
uint32_t hda_format_rate(uint16_t fmt);
uint32_t hda_format_channels(uint16_t fmt);
uint32_t hda_format_bits(uint16_t fmt);

/* hda_controller.c: 1 when the controller reads all-ones (gone). */
int hda_ctrl_dead(struct hda_controller *ctrl);
/* hda_controller.c: an engine started (1) or stopped (0); arms or drops
 * the interrupt watchdog / the timer that replaces a missing interrupt.
 * A no-op for controllers this driver does not own. */
void hda_ctrl_stream_event(struct hda_stream *st, int running);

/* ---- extra entry points for other drivers (hda_controller.c) ------------------ */

/* For the SOF driver: does the audio DSP own this controller's internal
 * audio?  1 = the controller has a DSP (prog-if 0x80) and no analog codec
 * answered on the HDA link, so speakers and microphones hang off
 * SoundWire/DMIC/SSP behind the DSP and need SOF; the HDA driver keeps the
 * link up for the display (HDMI/DP) codecs.  0 = the HDA driver drives the
 * analog audio itself (SOF must leave the device alone), or the function
 * has no DSP.  -ENODEV = the HDA driver did not take this device. */
int hda_controller_dsp_owned(const pci_device_t *pci);
/* For the SOF driver taking a controller over: stop all DMA, disable and
 * free the interrupt, disconnect the HDA card.  0 or -ENODEV. */
int hda_controller_release(const pci_device_t *pci);
/* For the display driver (Haswell/Broadwell display HDA): the current
 * CDCLK in kHz, so the link BCLK M/N divider can be reprogrammed. */
void hda_controller_set_cdclk(int cdclk_khz);

#endif /* KERNEL_DEV_SOUND_HDA_REGS_H */
