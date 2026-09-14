// LikeOS-64 -- HDMI and DVI on the DDI ports.
//
// A TMDS sink is simpler than DisplayPort: no link training, no
// bandwidth negotiation -- the port's clock is the pixel clock, which a
// WRPLL synthesises, and the transcoder streams the picture at that
// rate.  What is left is finding the sink (the hotplug pin, then its
// EDID over DDC through the GMBUS controller), telling an HDMI sink what
// it is getting (the AVI infoframe, sent as a data-island packet in the
// blanking), and the voltage settings of the transmitter.  A DVI sink
// gets the same stream without packets.
#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/dev/gpu/drm_edid.h>
#include <kernel/dev/gpu/i915/intel_infoframe.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

#define HDMI_MAX_CLOCK_KHZ 300000 /* Gen9 TMDS without scrambling */
#define DVI_MAX_CLOCK_KHZ 165000 /* single-link */
#define HDMI_MIN_CLOCK_KHZ 25000

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
	if (!intel_hpd_live(i915, o->port)) {
		o->detected = 0;
		o->edid_len = 0;
		o->hdmi_sink = 0;
		return 0;
	}
	/* Something pulls the pin; a TMDS sink is only usable with its
	 * EDID, so that decides. */
	o->detected = intel_hdmi_read_edid(i915, o) == 0;
	return o->detected;
}

int intel_hdmi_mode_valid(struct i915_device *i915, struct intel_output *o,
			  const struct drm_mode_modeinfo *m)
{
	(void)i915;
	uint32_t max = (o->type == INTEL_OUTPUT_HDMI && o->hdmi_sink) ? HDMI_MAX_CLOCK_KHZ :
									  DVI_MAX_CLOCK_KHZ;
	if (m->clock < HDMI_MIN_CLOCK_KHZ || m->clock > max)
		return -EINVAL;
	if (m->flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLCLK | DRM_MODE_FLAG_DBLSCAN))
		return -EINVAL;
	return 0;
}

/* ---- the transmitter ------------------------------------------------------------ */

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

int intel_hdmi_pre_enable(struct i915_device *i915, struct intel_output *o,
			  const struct drm_mode_modeinfo *m, int transcoder)
{
	o->pll = intel_dpll_get_hdmi(i915, o->port, m->clock);
	if (o->pll < 0) {
		kprintf("[drm] i915: port %c: no PLL for a %u kHz TMDS clock\n",
			'A' + o->port, m->clock);
		return -EIO;
	}
	intel_dpll_route_port(i915, o->port, o->pll);
	/* the transmitter's swing: the VBT's level shifter value, else the
	 * platform default */
	struct intel_vbt_port *vp = &i915->display.vbt.port[o->port];
	int level = (i915->display.vbt.valid && vp->hdmi_level_shift != 0xff) ?
			    vp->hdmi_level_shift : -1;
	intel_ddi_prepare_hdmi(i915, o, level);
	/* the packets: only an HDMI sink reads them */
	i915_write32(i915, HSW_TVIDEO_DIP_CTL(transcoder), 0);
	if (o->type == INTEL_OUTPUT_HDMI && o->hdmi_sink) {
		uint8_t avi[17];
		int n = intel_hdmi_avi_infoframe(m, intel_hdmi_vic_for_mode(m), avi, sizeof(avi));
		if (n > 0)
			dip_write(i915, transcoder, VIDEO_DIP_ENABLE_AVI_HSW,
				  HSW_TVIDEO_DIP_AVI_DATA(transcoder, 0), avi, (unsigned)n);
	}
	return 0;
}

void intel_hdmi_enable(struct i915_device *i915, struct intel_output *o)
{
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
	(void)o;
	i915_write32(i915, HSW_TVIDEO_DIP_CTL(transcoder), 0);
	(void)i915_read32(i915, HSW_TVIDEO_DIP_CTL(transcoder));
}

void intel_hdmi_post_disable(struct i915_device *i915, struct intel_output *o)
{
	intel_ddi_buf_disable(i915, o);
}
