// LikeOS -- HDMI and DVI on the DDI ports.
//
// A TMDS sink is simpler than DisplayPort: no link training, no
// bandwidth negotiation -- the port's clock is the pixel clock, which a
// WRPLL synthesises, and the transcoder streams the picture at that
// rate.  What is left is finding the sink (the hotplug pin, then its
// EDID over DDC through the GMBUS controller), telling an HDMI sink what
// it is getting (the AVI infoframe, sent as a data-island packet in the
// blanking), and the voltage settings of the transmitter.  A DVI sink
// gets the same stream without packets.
//
// The HDMI 2.0 side -- TMDS clocks above 340 MHz with scrambling, the
// limits of the sink and of a DP++ adaptor in between -- is
// intel_hdmi_feat.c's; detection, the mode check and the enable and
// disable steps here call into it.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2006 Dave Airlie <airlied@linux.ie>
// Portions Copyright (C) 2006-2009 Intel Corporation
// Portions Copyright (C) 2012 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/intel_dpll_regs.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/dev/gpu/i915/intel_features.h>
#include <kernel/dev/gpu/i915/intel_hdmi_feat.h>
#include <kernel/dev/gpu/drm_edid.h>
#include <kernel/dev/gpu/i915/intel_infoframe.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>


/* ---- detection ---------------------------------------------------------------- */

int intel_hdmi_read_edid(struct i915_device *i915, struct intel_output *o)
{
	(void)i915;
	int blocks = drm_edid_read(&o->ddc, o->edid, (int)(sizeof(o->edid) / DRM_EDID_BLOCK));
	if (blocks <= 0) {
		o->edid_len = 0;
		o->hdmi_sink = 0;
		return blocks < 0 ? blocks : -ENOENT;
	}
	o->edid_len = blocks * DRM_EDID_BLOCK;
	struct drm_edid_info info;
	o->hdmi_sink = drm_edid_parse(o->edid, (unsigned)o->edid_len, &info) == 0 && info.is_hdmi;
	return 0;
}

int intel_hdmi_detect(struct i915_device *i915, struct intel_output *o)
{
	/* an adaptor seen before is looked for again */
	intel_hdmi_dual_mode_reset(o);
	if (!intel_hpd_live(i915, o->port)) {
		o->detected = 0;
		o->edid_len = 0;
		o->hdmi_sink = 0;
		return 0;
	}
	/* Something pulls the pin; a TMDS sink is only usable with its
	 * EDID, so that decides. */
	o->detected = intel_hdmi_read_edid(i915, o) == 0;
	/* A digital sink: perhaps through a DP++ adaptor, whose type and
	 * TMDS limit its registers on the same bus tell. */
	if (o->detected && o->edid_len >= DRM_EDID_BLOCK && (o->edid[20] & 0x80))
		intel_hdmi_dual_mode_detect(i915, o);
	return o->detected;
}

int intel_hdmi_mode_valid(struct i915_device *i915, struct intel_output *o,
			  const struct drm_mode_modeinfo *m)
{
	int hdmi_sink = (o->type == INTEL_OUTPUT_HDMI && o->hdmi_sink);
	uint32_t max;

	if (m->flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLCLK | DRM_MODE_FLAG_DBLSCAN))
		return -EINVAL;
	if (I915_FEAT_HDMI_SCRAMBLING) {
		/* The source's limit (the platform's, the board's), the
		 * adaptor's and the sink's; 340 MHz unless both ends
		 * scramble.  Nothing needs FRL here, so 600 MHz is the top
		 * whatever the sink takes. */
		max = intel_hdmi_port_clock_limit(i915, o, hdmi_sink);
		if (max > 600000)
			max = 600000;
	} else {
		/* the platform's TMDS ceiling, without the scrambling HDMI
		 * 2.0 needs above 340 MHz; a DVI sink stops at single link */
		max = intel_dpll_hdmi_max_tmds_khz(i915);
		if (max > 340000)
			max = 340000;
		if (!hdmi_sink && max > 165000)
			max = 165000;
		if (I915_FEAT_DP_DUAL_MODE && o->hdmi_link.dual_mode_max_tmds_khz &&
		    o->hdmi_link.dual_mode_max_tmds_khz < max)
			max = o->hdmi_link.dual_mode_max_tmds_khz;
	}
	/* then the frequencies the port's PLL cannot synthesise */
	if (m->clock < 25000 || m->clock > max || intel_dpll_hdmi_clock_valid(i915, o, m->clock))
		return -EINVAL;
	return 0;
}

/* ---- the transmitter ------------------------------------------------------------ */

/* One data island packet into transcoder t's buffer: its enable bit off,
 * the data written -- every byte of the buffer, the unused ones as zero,
 * so the hardware computes the right ECC -- and the bit on again. */
static void dip_write(struct i915_device *i915, int t, uint32_t enable_bit,
		      uint32_t data_reg0, const uint8_t *frame, unsigned len)
{
	uint32_t ctl = i915_read32(i915, HSW_TVIDEO_DIP_CTL(t));
	ctl &= ~enable_bit;
	i915_write32(i915, HSW_TVIDEO_DIP_CTL(t), ctl);
	unsigned i;
	for (i = 0; i < VIDEO_DIP_DATA_SIZE; i += 4) {
		uint32_t v = 0;
		for (unsigned b = 0; b < 4; b++)
			if (i + b < len)
				v |= (uint32_t)frame[i + b] << (8 * b);
		i915_write32(i915, data_reg0 + i, v);
	}
	ctl |= enable_bit;
	i915_write32(i915, HSW_TVIDEO_DIP_CTL(t), ctl);
	(void)i915_read32(i915, HSW_TVIDEO_DIP_CTL(t));
}

/* The driver's own AVI infoframe, alone. */
static void infoframes_own(struct i915_device *i915, const struct drm_mode_modeinfo *m,
			   int transcoder)
{
	uint8_t avi[17];
	int n = intel_hdmi_avi_infoframe(m, intel_hdmi_vic_for_mode(m), avi, sizeof(avi));
	if (n > 0)
		dip_write(i915, transcoder, VIDEO_DIP_ENABLE_AVI_HSW,
			  HSW_TVIDEO_DIP_AVI_DATA(transcoder, 0), avi, (unsigned)n);
}

/* The infoframe library's packets for the sink on the output's
 * connector: general control first (its register, then its enable bit),
 * then AVI, product description and HDMI vendor infoframes.  A mode the
 * AVI infoframe cannot describe gets the driver's own AVI frame rather
 * than a dark screen.  The transcoder's DDI function is still off. */
static void infoframes_lib(struct i915_device *i915, struct intel_output *o,
			   const struct drm_mode_modeinfo *m, int transcoder)
{
	struct intel_hdmi_infoframes *f = kalloc(sizeof(*f));
	const struct drm_connector *c = NULL;
	int rc;

	if (o->conn >= 0 && (uint32_t)o->conn < i915->drm.nconn)
		c = &i915->drm.conn[o->conn];
	rc = f ? intel_hdmi_infoframes_compute(c, m, !!(i915->info->flags & I915_INFO_IS_DGFX), f) :
		 -ENOMEM;
	if (rc) {
		static unsigned said;
		if (said < 4) {
			said++;
			kprintf("[drm] i915: port %c: no AVI infoframe for %ux%u from the library (%d); sending the driver's own\n",
				'A' + o->port, m->hdisplay, m->vdisplay, rc);
		}
		if (f)
			kfree(f);
		infoframes_own(i915, m, transcoder);
		return;
	}
	if (f->gcp_enable) {
		i915_write32(i915, HSW_TVIDEO_DIP_GCP(transcoder), f->gcp);
		i915_write32(i915, HSW_TVIDEO_DIP_CTL(transcoder),
			     i915_read32(i915, HSW_TVIDEO_DIP_CTL(transcoder)) | VIDEO_DIP_ENABLE_GCP_HSW);
		(void)i915_read32(i915, HSW_TVIDEO_DIP_CTL(transcoder));
	}
	if (f->avi_len > 0)
		dip_write(i915, transcoder, VIDEO_DIP_ENABLE_AVI_HSW,
			  HSW_TVIDEO_DIP_AVI_DATA(transcoder, 0), f->avi, (unsigned)f->avi_len);
	if (f->spd_len > 0)
		dip_write(i915, transcoder, VIDEO_DIP_ENABLE_SPD_HSW,
			  HSW_TVIDEO_DIP_SPD_DATA(transcoder, 0), f->spd, (unsigned)f->spd_len);
	if (f->vendor_len > 0)
		dip_write(i915, transcoder, VIDEO_DIP_ENABLE_VS_HSW,
			  HSW_TVIDEO_DIP_VS_DATA(transcoder, 0), f->vendor, (unsigned)f->vendor_len);
	kfree(f);
}

int intel_hdmi_pre_enable(struct i915_device *i915, struct intel_output *o,
			  const struct drm_mode_modeinfo *m, int transcoder)
{
	/* a DP++ type 2 adaptor's output buffers on first */
	intel_hdmi_dual_mode_set_tmds_output(i915, o, 1);
	o->pll = intel_dpll_get_hdmi(i915, o->port, m->clock);
	if (o->pll < 0) {
		kprintf("[drm] i915: port %c: no PLL for a %u kHz TMDS clock\n",
			'A' + o->port, m->clock);
		return -EIO;
	}
	/* A Type-C PHY carrying TMDS is set up for all four lanes first, the
	 * way it is for a DisplayPort link. */
	if ((i915->display.model == INTEL_DISPLAY_ICL || i915->display.model == INTEL_DISPLAY_TGL) &&
	    o->is_tc)
		intel_tc_program_dp_mode(i915, o, 4);
	intel_dpll_route_port(i915, o->port, o->pll);
	/* the transmitter's swing: the VBT's level shifter value, else the
	 * platform default */
	struct intel_vbt_port *vp = &i915->display.vbt.port[o->port];
	int level = (i915->display.vbt.valid && vp->hdmi_level_shift != 0xff) ?
			    vp->hdmi_level_shift : -1;
	intel_ddi_prepare_hdmi(i915, o, level);
	/* scrambling and the clock ratio, for the transcoder's DDI
	 * function and the sink (8 bits per component: the TMDS clock is
	 * the pixel clock) */
	intel_hdmi_link_compute(i915, o, m->clock);
	/* the packets: only an HDMI sink reads them */
	i915_write32(i915, HSW_TVIDEO_DIP_CTL(transcoder), 0);
	if (o->type == INTEL_OUTPUT_HDMI && o->hdmi_sink) {
		if (I915_FEAT_HDMI_INFOFRAME_LIB)
			infoframes_lib(i915, o, m, transcoder);
		else
			infoframes_own(i915, m, transcoder);
	}
	return 0;
}

void intel_hdmi_enable(struct i915_device *i915, struct intel_output *o)
{
	/* Meteor Lake's buffer is the PHY's as much as the port's. */
	if (i915->display.model == INTEL_DISPLAY_MTL) {
		mtl_ddi_buf_enable(i915, o, 4);
		return;
	}
	if (i915->display.model == INTEL_DISPLAY_DG2) {
		dg2_ddi_buf_enable(i915, o, 4);
		return;
	}
	/* In TMDS mode the port width and swing selects are not used;
	 * enabling the buffer is all. */
	uint32_t v = i915_read32(i915, DDI_BUF_CTL(o->port));
	v &= ~(DDI_PORT_WIDTH_MASK | DDI_BUF_EMP_MASK | DDI_BUF_PORT_REVERSAL);
	if (o->lane_reversal)
		v |= DDI_BUF_PORT_REVERSAL;
	v |= DDI_BUF_CTL_ENABLE;
	i915_write32(i915, DDI_BUF_CTL(o->port), v);
	(void)i915_read32(i915, DDI_BUF_CTL(o->port));
	for (int t = 0; t < 100; t++) {
		if (!(i915_read32(i915, DDI_BUF_CTL(o->port)) & DDI_BUF_IS_IDLE))
			break;
		lapic_delay_us(10);
	}
}

void intel_hdmi_disable(struct i915_device *i915, struct intel_output *o,
			int transcoder)
{
	/* the sink back to an unscrambled 1/10 link, while the stream
	 * still runs */
	intel_hdmi_link_disable(i915, o);
	i915_write32(i915, HSW_TVIDEO_DIP_CTL(transcoder), 0);
	(void)i915_read32(i915, HSW_TVIDEO_DIP_CTL(transcoder));
}

void intel_hdmi_post_disable(struct i915_device *i915, struct intel_output *o)
{
	intel_ddi_buf_disable(i915, o);
	intel_hdmi_link_clear(o);
	/* and a DP++ type 2 adaptor's output buffers off */
	intel_hdmi_dual_mode_set_tmds_output(i915, o, 0);
}
