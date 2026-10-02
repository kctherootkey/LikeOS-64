// LikeOS -- SDVO encoder chips on the gen3/gen4 and Ironlake to Ivy Bridge ports.
//
// SDVO is a serialised version of the DVO bus: the pipe sends its pixels
// over a high-speed differential link to an external encoder chip that
// turns them into TMDS (DVI or HDMI), analog VGA, LVDS for a panel, or a
// television signal.  The chip sits on port B or C of the GMCH parts
// (only B on the PCH parts) and is controlled through an I2C bus of its
// own: commands are written as up to eight argument bytes and an opcode,
// the chip answers with a status byte (often "pending" for a while first)
// and up to eight return bytes.  The same bus reaches the monitor's DDC
// behind the chip once a bus-switch command opens the way, so EDID reads
// go through a proxy that prepends that command.  The chip wants the
// link clock between 100 and 200 MHz, so slow modes run the link at two
// or four times the pixel clock and the chip divides it down again;
// panels and televisions get an input timing the chip asks for, from
// which it scales onto the output timing itself.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2006-2010 Intel Corporation
// Portions Copyright (C) 2006 Dave Airlie

#include <kernel/dev/gpu/i915/intel_legacy.h>
#include <kernel/dev/gpu/drm_edid.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

/* ---- the command protocol --------------------------------------------------- */

/* Output flags: the functions an encoder chip may have. */
#define SDVO_OUTPUT_TMDS0 (1u << 0)
#define SDVO_OUTPUT_RGB0 (1u << 1)
#define SDVO_OUTPUT_CVBS0 (1u << 2)
#define SDVO_OUTPUT_SVID0 (1u << 3)
#define SDVO_OUTPUT_YPRPB0 (1u << 4)
#define SDVO_OUTPUT_SCART0 (1u << 5)
#define SDVO_OUTPUT_LVDS0 (1u << 6)
#define SDVO_OUTPUT_TMDS1 (1u << 8)
#define SDVO_OUTPUT_RGB1 (1u << 9)
#define SDVO_OUTPUT_CVBS1 (1u << 10)
#define SDVO_OUTPUT_SVID1 (1u << 11)
#define SDVO_OUTPUT_YPRPB1 (1u << 12)
#define SDVO_OUTPUT_SCART1 (1u << 13)
#define SDVO_OUTPUT_LVDS1 (1u << 14)

#define SDVO_TMDS_MASK (SDVO_OUTPUT_TMDS0 | SDVO_OUTPUT_TMDS1)
#define SDVO_RGB_MASK (SDVO_OUTPUT_RGB0 | SDVO_OUTPUT_RGB1)
#define SDVO_LVDS_MASK (SDVO_OUTPUT_LVDS0 | SDVO_OUTPUT_LVDS1)
#define SDVO_TV_MASK (SDVO_OUTPUT_CVBS0 | SDVO_OUTPUT_SVID0 | SDVO_OUTPUT_YPRPB0)
#define SDVO_OUTPUT_MASK (SDVO_TMDS_MASK | SDVO_RGB_MASK | SDVO_LVDS_MASK | SDVO_TV_MASK)

#define SDVO_IS_TV(f) (((f) & SDVO_TV_MASK) != 0)
#define SDVO_IS_TMDS(f) (((f) & SDVO_TMDS_MASK) != 0)
#define SDVO_IS_LVDS(f) (((f) & SDVO_LVDS_MASK) != 0)
#define SDVO_IS_DIGITAL(f) (((f) & (SDVO_TMDS_MASK | SDVO_LVDS_MASK)) != 0)
#define SDVO_HAS_DDC(f) (((f) & (SDVO_RGB_MASK | SDVO_TMDS_MASK | SDVO_LVDS_MASK)) != 0)

/* The chip's I2C registers. */
#define SDVO_I2C_ARG_0 0x07
#define SDVO_I2C_OPCODE 0x08
#define SDVO_I2C_CMD_STATUS 0x09
#define SDVO_I2C_RETURN_0 0x0a

/* Status byte values */
#define SDVO_CMD_STATUS_POWER_ON 0x0
#define SDVO_CMD_STATUS_SUCCESS 0x1
#define SDVO_CMD_STATUS_NOTSUPP 0x2
#define SDVO_CMD_STATUS_INVALID_ARG 0x3
#define SDVO_CMD_STATUS_PENDING 0x4
#define SDVO_CMD_STATUS_TARGET_NOT_SPECIFIED 0x5
#define SDVO_CMD_STATUS_SCALING_NOT_SUPP 0x6

/* Opcodes */
#define SDVO_CMD_RESET 0x01
#define SDVO_CMD_GET_DEVICE_CAPS 0x02
#define SDVO_CMD_GET_TRAINED_INPUTS 0x03
#define SDVO_CMD_GET_ACTIVE_OUTPUTS 0x04
#define SDVO_CMD_SET_ACTIVE_OUTPUTS 0x05
#define SDVO_CMD_GET_IN_OUT_MAP 0x06
#define SDVO_CMD_SET_IN_OUT_MAP 0x07
#define SDVO_CMD_GET_ATTACHED_DISPLAYS 0x0b
#define SDVO_CMD_GET_HOT_PLUG_SUPPORT 0x0c
#define SDVO_CMD_SET_ACTIVE_HOT_PLUG 0x0d
#define SDVO_CMD_GET_ACTIVE_HOT_PLUG 0x0e
#define SDVO_CMD_SET_TARGET_INPUT 0x10
#define SDVO_CMD_SET_TARGET_OUTPUT 0x11
#define SDVO_CMD_GET_INPUT_TIMINGS_PART1 0x12
#define SDVO_CMD_GET_INPUT_TIMINGS_PART2 0x13
#define SDVO_CMD_SET_INPUT_TIMINGS_PART1 0x14
#define SDVO_CMD_SET_INPUT_TIMINGS_PART2 0x15
#define SDVO_CMD_SET_OUTPUT_TIMINGS_PART1 0x16
#define SDVO_CMD_SET_OUTPUT_TIMINGS_PART2 0x17
#define SDVO_CMD_GET_OUTPUT_TIMINGS_PART1 0x18
#define SDVO_CMD_GET_OUTPUT_TIMINGS_PART2 0x19
#define SDVO_CMD_CREATE_PREFERRED_INPUT_TIMING 0x1a
#define SDVO_CMD_GET_PREFERRED_INPUT_TIMING_PART1 0x1b
#define SDVO_CMD_GET_PREFERRED_INPUT_TIMING_PART2 0x1c
#define SDVO_CMD_GET_INPUT_PIXEL_CLOCK_RANGE 0x1d
#define SDVO_CMD_GET_OUTPUT_PIXEL_CLOCK_RANGE 0x1e
#define SDVO_CMD_GET_SUPPORTED_CLOCK_RATE_MULTS 0x1f
#define SDVO_CMD_GET_CLOCK_RATE_MULT 0x20
#define SDVO_CMD_SET_CLOCK_RATE_MULT 0x21
#define SDVO_CMD_GET_SUPPORTED_TV_FORMATS 0x27
#define SDVO_CMD_GET_TV_FORMAT 0x28
#define SDVO_CMD_SET_TV_FORMAT 0x29
#define SDVO_CMD_GET_SUPPORTED_POWER_STATES 0x2a
#define SDVO_CMD_GET_ENCODER_POWER_STATE 0x2b
#define SDVO_CMD_SET_ENCODER_POWER_STATE 0x2c
#define SDVO_CMD_SET_CONTROL_BUS_SWITCH 0x7a
#define SDVO_CMD_SET_DISPLAY_POWER_STATE 0x7d
#define SDVO_CMD_GET_SDTV_RESOLUTION_SUPPORT 0x83
#define SDVO_CMD_GET_SUPPORTED_ENHANCEMENTS 0x84
#define SDVO_CMD_GET_SCALED_HDTV_RESOLUTION_SUPPORT 0x85
#define SDVO_CMD_GET_FIRMWARE_REV 0x86
#define SDVO_CMD_SET_PIXEL_REPLI 0x8b
#define SDVO_CMD_GET_PIXEL_REPLI 0x8c
#define SDVO_CMD_GET_COLORIMETRY_CAP 0x8d
#define SDVO_CMD_SET_COLORIMETRY 0x8e
#define SDVO_CMD_GET_COLORIMETRY 0x8f
#define SDVO_CMD_GET_AUDIO_ENCRYPT_PREFER 0x90
#define SDVO_CMD_SET_AUDIO_STAT 0x91
#define SDVO_CMD_GET_AUDIO_STAT 0x92
#define SDVO_CMD_SET_HBUF_INDEX 0x93
#define SDVO_CMD_GET_HBUF_INDEX 0x94
#define SDVO_CMD_GET_HBUF_INFO 0x95
#define SDVO_CMD_SET_HBUF_AV_SPLIT 0x96
#define SDVO_CMD_GET_HBUF_AV_SPLIT 0x97
#define SDVO_CMD_SET_HBUF_DATA 0x98
#define SDVO_CMD_GET_HBUF_DATA 0x99
#define SDVO_CMD_SET_HBUF_TXRATE 0x9a
#define SDVO_CMD_GET_HBUF_TXRATE 0x9b
#define SDVO_CMD_GET_SUPP_ENCODE 0x9d
#define SDVO_CMD_GET_ENCODE 0x9e
#define SDVO_CMD_SET_ENCODE 0x9f

#define SDVO_CLOCK_RATE_MULT_1X (1u << 0)
#define SDVO_CLOCK_RATE_MULT_2X (1u << 1)
#define SDVO_CLOCK_RATE_MULT_4X (1u << 3)

#define SDVO_ENCODER_STATE_ON (1u << 0)
#define SDVO_ENCODER_STATE_OFF (1u << 3)

#define SDVO_ENCODE_DVI 0x0
#define SDVO_ENCODE_HDMI 0x1

#define SDVO_COLORIMETRY_RGB256 (1u << 0)
#define SDVO_COLORIMETRY_RGB220 (1u << 1)

#define SDVO_AUDIO_ELD_VALID (1u << 0)
#define SDVO_AUDIO_PRESENCE_DETECT (1u << 1)

#define SDVO_HBUF_INDEX_ELD 0
#define SDVO_HBUF_INDEX_AVI_IF 1
#define SDVO_HBUF_TX_DISABLED (0u << 6)
#define SDVO_HBUF_TX_ONCE (2u << 6)
#define SDVO_HBUF_TX_VSYNC (3u << 6)

/* In the sdvo_flags byte of an input timing: the chip needs the port to
 * stall the pipe now and then (scaling panels and televisions). */
#define SDVO_NEED_TO_STALL (1u << 7)

/* Detailed timing flags (the EDID's) */
#define SDVO_DTD_FLAG_HSYNC_POSITIVE (1u << 1)
#define SDVO_DTD_FLAG_VSYNC_POSITIVE (1u << 2)
#define SDVO_DTD_FLAG_INTERLACE (1u << 7)

#define SDVO_PREFERRED_INPUT_TIMING_FLAGS_INTERLACED (1u << 0)
#define SDVO_PREFERRED_INPUT_TIMING_FLAGS_SCALED (1u << 1)

/* The capability reply (GET_DEVICE_CAPS): 8 bytes. */
struct sdvo_caps {
	uint8_t vendor_id;
	uint8_t device_id;
	uint8_t device_rev_id;
	uint8_t sdvo_version_major;
	uint8_t sdvo_version_minor;
	uint8_t bits; /* num_inputs:2, smooth, sharp, up, down, stall, pad */
	uint16_t output_flags;
} __attribute__((packed));

#define SDVO_CAPS_NUM_INPUTS(c) ((c)->bits & 3u)
#define SDVO_CAPS_SMOOTH_SCALING(c) (((c)->bits >> 2) & 1u)
#define SDVO_CAPS_SHARP_SCALING(c) (((c)->bits >> 3) & 1u)
#define SDVO_CAPS_UP_SCALING(c) (((c)->bits >> 4) & 1u)
#define SDVO_CAPS_DOWN_SCALING(c) (((c)->bits >> 5) & 1u)
#define SDVO_CAPS_STALL(c) (((c)->bits >> 6) & 1u)

/* A detailed timing as the chip takes it, in two 8-byte halves (the
 * EDID's detailed timing descriptor, more or less). */
struct sdvo_dtd {
	struct {
		uint16_t clock; /* 10 kHz units */
		uint8_t h_active; /* low 8 bits */
		uint8_t h_blank; /* low 8 bits */
		uint8_t h_high; /* high 4 bits each of h_active, h_blank */
		uint8_t v_active;
		uint8_t v_blank;
		uint8_t v_high;
	} __attribute__((packed)) part1;
	struct {
		uint8_t h_sync_off; /* low 8 bits, from the blank start */
		uint8_t h_sync_width;
		uint8_t v_sync_off_width; /* low 4 bits each */
		uint8_t sync_off_width_high;
		uint8_t dtd_flags;
		uint8_t sdvo_flags;
		uint8_t v_sync_off_high; /* bits 6-7 of the vsync offset */
		uint8_t reserved;
	} __attribute__((packed)) part2;
} __attribute__((packed));

struct sdvo_pixel_clock_range {
	uint16_t min; /* 10 kHz units */
	uint16_t max;
} __attribute__((packed));

struct sdvo_preferred_input_timing_args {
	uint16_t clock;
	uint16_t width;
	uint16_t height;
	uint8_t flags; /* SDVO_PREFERRED_INPUT_TIMING_FLAGS_* */
} __attribute__((packed));

struct sdvo_in_out_map {
	uint16_t in0, in1;
} __attribute__((packed));

/* The television standards, in the order of the format bitmap. */
static const char *const sdvo_tv_format_names[] = {
	"NTSC_M", "NTSC_J", "NTSC_443", "PAL_B", "PAL_D", "PAL_G", "PAL_H",
	"PAL_I", "PAL_M", "PAL_N", "PAL_NC", "PAL_60", "SECAM_B", "SECAM_D",
	"SECAM_G", "SECAM_K", "SECAM_K1", "SECAM_L", "SECAM_60",
};
#define SDVO_TV_FORMAT_NUM ((int)(sizeof(sdvo_tv_format_names) / sizeof(sdvo_tv_format_names[0])))

/* The input resolutions a television encoder may offer for a standard,
 * in the bit order of the GET_SDTV_RESOLUTION_SUPPORT reply. */
static const struct {
	uint32_t clock;
	uint16_t hd, hss, hse, ht;
	uint16_t vd, vss, vse, vt;
} sdvo_tv_modes[] = {
	{ 5815, 320, 321, 384, 416, 200, 201, 232, 233 },
	{ 6814, 320, 321, 384, 416, 240, 241, 272, 273 },
	{ 9910, 400, 401, 464, 496, 300, 301, 332, 333 },
	{ 16913, 640, 641, 704, 736, 350, 351, 382, 383 },
	{ 19121, 640, 641, 704, 736, 400, 401, 432, 433 },
	{ 22654, 640, 641, 704, 736, 480, 481, 512, 513 },
	{ 24624, 704, 705, 768, 800, 480, 481, 512, 513 },
	{ 29232, 704, 705, 768, 800, 576, 577, 608, 609 },
	{ 18751, 720, 721, 784, 816, 350, 351, 382, 383 },
	{ 21199, 720, 721, 784, 816, 400, 401, 432, 433 },
	{ 25116, 720, 721, 784, 816, 480, 481, 512, 513 },
	{ 28054, 720, 721, 784, 816, 540, 541, 572, 573 },
	{ 29816, 720, 721, 784, 816, 576, 577, 608, 609 },
	{ 31570, 768, 769, 832, 864, 576, 577, 608, 609 },
	{ 34030, 800, 801, 864, 896, 600, 601, 632, 633 },
	{ 36581, 832, 833, 896, 928, 624, 625, 656, 657 },
	{ 48707, 920, 921, 984, 1016, 766, 767, 798, 799 },
	{ 53827, 1024, 1025, 1088, 1120, 768, 769, 800, 801 },
	{ 87265, 1280, 1281, 1344, 1376, 1024, 1025, 1056, 1057 },
};
#define SDVO_TV_MODES_NUM ((int)(sizeof(sdvo_tv_modes) / sizeof(sdvo_tv_modes[0])))

/* ---- state ------------------------------------------------------------------- */

struct sdvo_dev;

/* One of the chip's three DDC buses, reached through the control bus. */
struct sdvo_ddc {
	struct i2c_adapter adap;
	struct sdvo_dev *dev;
	uint8_t bus; /* 1..3: SDVO_CONTROL_BUS_DDC1..3 */
};

/* One encoder chip, shared by the outputs it drives. */
struct sdvo_dev {
	struct lg_display *d;
	int port; /* LG_PORT_B or LG_PORT_C */
	uint32_t reg; /* GEN3_SDVOB/C or PCH_SDVOB */
	/* the control bus */
	uint8_t i2c_pin;
	struct i2c_adapter *i2c;
	uint8_t addr; /* 7-bit */
	struct sdvo_ddc ddc[3];
	struct sdvo_caps caps;
	uint8_t colorimetry_cap;
	/* the input pixel clock the chip takes, kHz */
	int pixel_clock_min, pixel_clock_max;
	/* the outputs whose hotplug interrupt is in use */
	uint16_t hotplug_active;
	/* The sdvo_flags of the last preferred input timing: they do not
	 * survive the trip into a mode and back. */
	uint8_t dtd_sdvo_flags;
	/* d->out index of the first output made for it */
	int first_out;
};

/* One output function of a chip (one lg_output). */
struct sdvo_output {
	struct sdvo_dev *dev;
	uint16_t flag; /* the SDVO_OUTPUT_* bit */
	int is_hdmi; /* the chip can encode HDMI */
	int hotplug; /* the chip signals hotplug for this output */
	int tv_format; /* index into sdvo_tv_format_names */
	struct i2c_adapter *ddc;
	uint32_t conn_type, enc_type;
	const char *kind;
	/* The timing the pipe runs, when the chip chose it (panel, TV). */
	struct drm_mode_modeinfo adjusted;
	int adjusted_valid;
	/* found while probing, handed to the output when it is made */
	struct drm_mode_modeinfo fixed;
	int fixed_valid;
	uint32_t mm_w, mm_h;
	uint8_t *edid;
	int edid_len;
};

/* The encoders found, to know who else uses a control bus pin. */
static struct sdvo_dev *g_sdvo[2];

static char sdvo_port_name(const struct sdvo_dev *s)
{
	return s->port == LG_PORT_B ? 'B' : 'C';
}

/* ---- control bus transfers -------------------------------------------------- */

/* The register write and read port of both SDVO registers: on the GMCH
 * parts write SDVOB and SDVOC alike (twice, the first write sometimes
 * does not stick -- the video BIOS does the same); on Ibex Peak the port
 * register may mask the first write, so write it twice too. */
static void sdvo_write_sdvox(struct sdvo_dev *s, uint32_t val)
{
	struct lg_display *d = s->d;
	uint32_t bval = val, cval = val;
	int i;

	if (d->pch != LG_PCH_NONE) {
		lg_wr(d, s->reg, val);
		lg_posting_read(d, s->reg);
		if (d->pch == LG_PCH_IBX) {
			lg_wr(d, s->reg, val);
			lg_posting_read(d, s->reg);
		}
		return;
	}

	if (s->port == LG_PORT_B)
		cval = lg_rd(d, GEN3_SDVOC);
	else
		bval = lg_rd(d, GEN3_SDVOB);

	for (i = 0; i < 2; i++) {
		lg_wr(d, GEN3_SDVOB, bval);
		lg_posting_read(d, GEN3_SDVOB);
		lg_wr(d, GEN3_SDVOC, cval);
		lg_posting_read(d, GEN3_SDVOC);
	}
}

static int sdvo_read_byte(struct sdvo_dev *s, uint8_t addr, uint8_t *ch)
{
	struct i2c_msg msgs[2];
	int ret;

	msgs[0].addr = s->addr;
	msgs[0].flags = 0;
	msgs[0].len = 1;
	msgs[0].buf = &addr;
	msgs[1].addr = s->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = 1;
	msgs[1].buf = ch;

	ret = i2c_transfer(s->i2c, msgs, 2);
	if (ret == 2)
		return 1;
	i915_dbg("[drm] i915: SDVO%c: i2c transfer returned %d\n", sdvo_port_name(s), ret);
	return 0;
}

/* The messages of one command: a two-byte register write per argument
 * (ARG_0 downwards), the opcode, then the status register's address and a
 * one-byte read of it -- the status read in the same transfer starts the
 * chip on the command.  `buf' holds 2 * args_len + 2 bytes.  Returns the
 * message count, args_len + 3. */
static int sdvo_build_cmd(struct sdvo_dev *s, uint8_t cmd, const void *args, int args_len,
			  struct i2c_msg *msgs, uint8_t *buf, uint8_t *status)
{
	int i;

	for (i = 0; i < args_len; i++) {
		msgs[i].addr = s->addr;
		msgs[i].flags = 0;
		msgs[i].len = 2;
		msgs[i].buf = buf + 2 * i;
		buf[2 * i + 0] = (uint8_t)(SDVO_I2C_ARG_0 - i);
		buf[2 * i + 1] = ((const uint8_t *)args)[i];
	}
	msgs[i].addr = s->addr;
	msgs[i].flags = 0;
	msgs[i].len = 2;
	msgs[i].buf = buf + 2 * i;
	buf[2 * i + 0] = SDVO_I2C_OPCODE;
	buf[2 * i + 1] = cmd;

	*status = SDVO_I2C_CMD_STATUS;
	msgs[i + 1].addr = s->addr;
	msgs[i + 1].flags = 0;
	msgs[i + 1].len = 1;
	msgs[i + 1].buf = status;

	msgs[i + 2].addr = s->addr;
	msgs[i + 2].flags = I2C_M_RD;
	msgs[i + 2].len = 1;
	msgs[i + 2].buf = status;

	return i + 3;
}

static void sdvo_debug_write(struct sdvo_dev *s, uint8_t cmd, const void *args, int args_len)
{
	char buffer[64];
	int i, pos = 0;

	if (!I915_DEBUG)
		return;
	for (i = 0; i < args_len && pos < (int)sizeof(buffer) - 4; i++)
		pos += ksnprintf(buffer + pos, sizeof(buffer) - (size_t)pos, "%02X ",
				 ((const uint8_t *)args)[i]);
	buffer[pos] = '\0';
	i915_dbg("[drm] i915: SDVO%c: W: %02X %s\n", sdvo_port_name(s), cmd, buffer);
}

static int sdvo_write_cmd(struct sdvo_dev *s, uint8_t cmd, const void *args, int args_len)
{
	struct i2c_msg msgs[8 + 3];
	uint8_t buf[2 * 8 + 2];
	uint8_t status;
	int n, ret;

	if (args_len < 0 || args_len > 8)
		return 0;

	sdvo_debug_write(s, cmd, args, args_len);

	n = sdvo_build_cmd(s, cmd, args, args_len, msgs, buf, &status);
	ret = i2c_transfer(s->i2c, msgs, n);
	if (ret < 0) {
		i915_dbg("[drm] i915: SDVO%c: I2c transfer returned %d\n", sdvo_port_name(s), ret);
		return 0;
	}
	if (ret != n) {
		i915_dbg("[drm] i915: SDVO%c: I2c transfer returned %d/%d\n", sdvo_port_name(s), ret,
			 n);
		return 0;
	}
	return 1;
}

/* Wait for the command to complete and read `response_len' return bytes.
 * Commands are specified to complete within 15 us with at most three
 * status polls; poll five times at that pace, then -- many chips, TV
 * encoders above all, say "pending" for much longer -- ten more times
 * 15 ms apart. */
static int sdvo_read_response(struct sdvo_dev *s, void *response, int response_len)
{
	uint8_t retry = 15;
	uint8_t status;
	int i;

	if (!sdvo_read_byte(s, SDVO_I2C_CMD_STATUS, &status))
		goto fail;

	while ((status == SDVO_CMD_STATUS_PENDING ||
		status == SDVO_CMD_STATUS_TARGET_NOT_SPECIFIED) && --retry) {
		if (retry < 10)
			lg_mdelay(15);
		else
			lg_udelay(15);

		if (!sdvo_read_byte(s, SDVO_I2C_CMD_STATUS, &status))
			goto fail;
	}

	if (status != SDVO_CMD_STATUS_SUCCESS) {
		i915_dbg("[drm] i915: SDVO%c: R: status %u\n", sdvo_port_name(s), status);
		return 0;
	}

	for (i = 0; i < response_len; i++) {
		if (!sdvo_read_byte(s, (uint8_t)(SDVO_I2C_RETURN_0 + i), &((uint8_t *)response)[i]))
			goto fail;
	}
	if (I915_DEBUG && response_len > 0) {
		char buffer[40];
		int pos = 0;

		for (i = 0; i < response_len && pos < (int)sizeof(buffer) - 4; i++)
			pos += ksnprintf(buffer + pos, sizeof(buffer) - (size_t)pos, " %02X",
					 ((uint8_t *)response)[i]);
		buffer[pos] = '\0';
		i915_dbg("[drm] i915: SDVO%c: R: (Success)%s\n", sdvo_port_name(s), buffer);
	}
	return 1;

fail:
	i915_dbg("[drm] i915: SDVO%c: R: ... failed\n", sdvo_port_name(s));
	return 0;
}

static int sdvo_set_value(struct sdvo_dev *s, uint8_t cmd, const void *data, int len)
{
	if (!sdvo_write_cmd(s, cmd, data, len))
		return 0;
	return sdvo_read_response(s, NULL, 0);
}

static int sdvo_get_value(struct sdvo_dev *s, uint8_t cmd, void *value, int len)
{
	if (!sdvo_write_cmd(s, cmd, NULL, 0))
		return 0;
	return sdvo_read_response(s, value, len);
}

/* ---- commands --------------------------------------------------------------- */

/* Input commands go to input 0: a single-input chip's only input, which
 * is the port the chip hangs off. */
static int sdvo_set_target_input(struct sdvo_dev *s)
{
	uint8_t targets = 0;

	return sdvo_set_value(s, SDVO_CMD_SET_TARGET_INPUT, &targets, 1);
}

static int sdvo_get_trained_inputs(struct sdvo_dev *s, int *input_1, int *input_2)
{
	uint8_t response;

	if (!sdvo_get_value(s, SDVO_CMD_GET_TRAINED_INPUTS, &response, 1))
		return 0;
	*input_1 = response & 1;
	*input_2 = (response >> 1) & 1;
	return 1;
}

static int sdvo_set_active_outputs(struct sdvo_dev *s, uint16_t outputs)
{
	return sdvo_set_value(s, SDVO_CMD_SET_ACTIVE_OUTPUTS, &outputs, 2);
}

static int sdvo_get_active_outputs(struct sdvo_dev *s, uint16_t *outputs)
{
	return sdvo_get_value(s, SDVO_CMD_GET_ACTIVE_OUTPUTS, outputs, 2);
}

static int sdvo_get_input_pixel_clock_range(struct sdvo_dev *s, int *clock_min, int *clock_max)
{
	struct sdvo_pixel_clock_range clocks;

	if (!sdvo_get_value(s, SDVO_CMD_GET_INPUT_PIXEL_CLOCK_RANGE, &clocks, sizeof(clocks)))
		return 0;
	/* 10 kHz units to kHz */
	*clock_min = clocks.min * 10;
	*clock_max = clocks.max * 10;
	return 1;
}

static int sdvo_set_target_output(struct sdvo_dev *s, uint16_t outputs)
{
	return sdvo_set_value(s, SDVO_CMD_SET_TARGET_OUTPUT, &outputs, 2);
}

static int sdvo_set_timing(struct sdvo_dev *s, uint8_t cmd, struct sdvo_dtd *dtd)
{
	return sdvo_set_value(s, cmd, &dtd->part1, sizeof(dtd->part1)) &&
	       sdvo_set_value(s, (uint8_t)(cmd + 1), &dtd->part2, sizeof(dtd->part2));
}

static int sdvo_set_input_timing(struct sdvo_dev *s, struct sdvo_dtd *dtd)
{
	return sdvo_set_timing(s, SDVO_CMD_SET_INPUT_TIMINGS_PART1, dtd);
}

static int sdvo_set_output_timing(struct sdvo_dev *s, struct sdvo_dtd *dtd)
{
	return sdvo_set_timing(s, SDVO_CMD_SET_OUTPUT_TIMINGS_PART1, dtd);
}

static int sdvo_set_clock_rate_mult(struct sdvo_dev *s, uint8_t val)
{
	return sdvo_set_value(s, SDVO_CMD_SET_CLOCK_RATE_MULT, &val, 1);
}

static int sdvo_set_encode(struct sdvo_dev *s, uint8_t mode)
{
	return sdvo_set_value(s, SDVO_CMD_SET_ENCODE, &mode, 1);
}

static int sdvo_set_colorimetry(struct sdvo_dev *s, uint8_t mode)
{
	return sdvo_set_value(s, SDVO_CMD_SET_COLORIMETRY, &mode, 1);
}

static int sdvo_set_pixel_replication(struct sdvo_dev *s, uint8_t pixel_repeat)
{
	return sdvo_set_value(s, SDVO_CMD_SET_PIXEL_REPLI, &pixel_repeat, 1);
}

/* Does the chip speak HDMI (it answers the encode query)? */
static int sdvo_check_supp_encode(struct sdvo_dev *s)
{
	uint8_t encode[2];

	return sdvo_get_value(s, SDVO_CMD_GET_SUPP_ENCODE, encode, 2);
}

static int sdvo_get_capabilities(struct sdvo_dev *s)
{
	struct sdvo_caps *caps = &s->caps;

	if (!sdvo_get_value(s, SDVO_CMD_GET_DEVICE_CAPS, caps, sizeof(*caps)))
		return 0;

	i915_dbg("[drm] i915: SDVO%c capabilities: vendor_id %u device_id %u rev %u version %u.%u "
		 "inputs %u smooth %u sharp %u up %u down %u stall %u output_flags 0x%x\n",
		 sdvo_port_name(s), caps->vendor_id, caps->device_id, caps->device_rev_id,
		 caps->sdvo_version_major, caps->sdvo_version_minor, SDVO_CAPS_NUM_INPUTS(caps),
		 SDVO_CAPS_SMOOTH_SCALING(caps), SDVO_CAPS_SHARP_SCALING(caps),
		 SDVO_CAPS_UP_SCALING(caps), SDVO_CAPS_DOWN_SCALING(caps), SDVO_CAPS_STALL(caps),
		 caps->output_flags);
	return 1;
}

static uint8_t sdvo_get_colorimetry_cap(struct sdvo_dev *s)
{
	uint8_t cap;

	if (!sdvo_get_value(s, SDVO_CMD_GET_COLORIMETRY_CAP, &cap, 1))
		return SDVO_COLORIMETRY_RGB256;
	return cap;
}

static uint16_t sdvo_get_hotplug_support(struct sdvo_dev *s)
{
	struct lg_display *d = s->d;
	uint16_t hotplug;

	/* gen2 and the i915G/GM have no hotplug detection. */
	if (d->ver < 3 || d->is_i915g || d->is_i915gm)
		return 0;
	/* The SDVO hotplug line of every i945G/GM is noisy: never trust it. */
	if (d->is_i945g || d->is_i945gm)
		return 0;
	if (!sdvo_get_value(s, SDVO_CMD_GET_HOT_PLUG_SUPPORT, &hotplug, 2))
		return 0;
	return hotplug;
}

/* Some chips' hotplug interrupts are one-shot: re-arm after every event.
 * The chip does not answer this command with a status. */
static void sdvo_enable_hotplug(struct sdvo_dev *s)
{
	if (!sdvo_write_cmd(s, SDVO_CMD_SET_ACTIVE_HOT_PLUG, &s->hotplug_active, 2))
		kprintf("[drm] i915: SDVO%c: failed to enable hotplug on the encoder\n",
			sdvo_port_name(s));
}

/* ---- timings ---------------------------------------------------------------- */

static void sdvo_get_dtd_from_mode(struct sdvo_dtd *dtd, const struct drm_mode_modeinfo *mode)
{
	uint16_t width, height;
	uint16_t h_blank_len, h_sync_len, v_blank_len, v_sync_len;
	uint16_t h_sync_offset, v_sync_offset;

	mm_memset(dtd, 0, sizeof(*dtd));

	width = mode->hdisplay;
	height = mode->vdisplay;

	h_blank_len = (uint16_t)(mode->htotal - mode->hdisplay);
	h_sync_len = (uint16_t)(mode->hsync_end - mode->hsync_start);
	v_blank_len = (uint16_t)(mode->vtotal - mode->vdisplay);
	v_sync_len = (uint16_t)(mode->vsync_end - mode->vsync_start);
	h_sync_offset = (uint16_t)(mode->hsync_start - mode->hdisplay);
	v_sync_offset = (uint16_t)(mode->vsync_start - mode->vdisplay);

	dtd->part1.clock = (uint16_t)(mode->clock / 10);

	dtd->part1.h_active = (uint8_t)(width & 0xff);
	dtd->part1.h_blank = (uint8_t)(h_blank_len & 0xff);
	dtd->part1.h_high = (uint8_t)((((width >> 8) & 0xf) << 4) | ((h_blank_len >> 8) & 0xf));
	dtd->part1.v_active = (uint8_t)(height & 0xff);
	dtd->part1.v_blank = (uint8_t)(v_blank_len & 0xff);
	dtd->part1.v_high = (uint8_t)((((height >> 8) & 0xf) << 4) | ((v_blank_len >> 8) & 0xf));

	dtd->part2.h_sync_off = (uint8_t)(h_sync_offset & 0xff);
	dtd->part2.h_sync_width = (uint8_t)(h_sync_len & 0xff);
	dtd->part2.v_sync_off_width = (uint8_t)(((v_sync_offset & 0xf) << 4) | (v_sync_len & 0xf));
	dtd->part2.sync_off_width_high =
		(uint8_t)(((h_sync_offset & 0x300) >> 2) | ((h_sync_len & 0x300) >> 4) |
			  ((v_sync_offset & 0x30) >> 2) | ((v_sync_len & 0x30) >> 4));

	dtd->part2.dtd_flags = 0x18;
	if (mode->flags & DRM_MODE_FLAG_INTERLACE)
		dtd->part2.dtd_flags |= SDVO_DTD_FLAG_INTERLACE;
	if (mode->flags & DRM_MODE_FLAG_PHSYNC)
		dtd->part2.dtd_flags |= SDVO_DTD_FLAG_HSYNC_POSITIVE;
	if (mode->flags & DRM_MODE_FLAG_PVSYNC)
		dtd->part2.dtd_flags |= SDVO_DTD_FLAG_VSYNC_POSITIVE;

	dtd->part2.v_sync_off_high = (uint8_t)(v_sync_offset & 0xc0);
}

static void sdvo_get_mode_from_dtd(struct drm_mode_modeinfo *pmode, const struct sdvo_dtd *dtd)
{
	struct drm_mode_modeinfo mode;

	mm_memset(&mode, 0, sizeof(mode));

	mode.hdisplay = dtd->part1.h_active;
	mode.hdisplay += ((dtd->part1.h_high >> 4) & 0x0f) << 8;
	mode.hsync_start = mode.hdisplay + dtd->part2.h_sync_off;
	mode.hsync_start += (dtd->part2.sync_off_width_high & 0xc0) << 2;
	mode.hsync_end = mode.hsync_start + dtd->part2.h_sync_width;
	mode.hsync_end += (dtd->part2.sync_off_width_high & 0x30) << 4;
	mode.htotal = mode.hdisplay + dtd->part1.h_blank;
	mode.htotal += (dtd->part1.h_high & 0xf) << 8;

	mode.vdisplay = dtd->part1.v_active;
	mode.vdisplay += ((dtd->part1.v_high >> 4) & 0x0f) << 8;
	mode.vsync_start = mode.vdisplay;
	mode.vsync_start += (dtd->part2.v_sync_off_width >> 4) & 0xf;
	mode.vsync_start += (dtd->part2.sync_off_width_high & 0x0c) << 2;
	mode.vsync_start += dtd->part2.v_sync_off_high & 0xc0;
	mode.vsync_end = mode.vsync_start + (dtd->part2.v_sync_off_width & 0xf);
	mode.vsync_end += (dtd->part2.sync_off_width_high & 0x3) << 4;
	mode.vtotal = mode.vdisplay + dtd->part1.v_blank;
	mode.vtotal += (dtd->part1.v_high & 0xf) << 8;

	mode.clock = (uint32_t)dtd->part1.clock * 10;

	if (dtd->part2.dtd_flags & SDVO_DTD_FLAG_INTERLACE)
		mode.flags |= DRM_MODE_FLAG_INTERLACE;
	if (dtd->part2.dtd_flags & SDVO_DTD_FLAG_HSYNC_POSITIVE)
		mode.flags |= DRM_MODE_FLAG_PHSYNC;
	else
		mode.flags |= DRM_MODE_FLAG_NHSYNC;
	if (dtd->part2.dtd_flags & SDVO_DTD_FLAG_VSYNC_POSITIVE)
		mode.flags |= DRM_MODE_FLAG_PVSYNC;
	else
		mode.flags |= DRM_MODE_FLAG_NVSYNC;

	mode.type = DRM_MODE_TYPE_DRIVER;
	drm_mode_finish(&mode);
	*pmode = mode;
}

/* The mode's refresh rate in Hz, rounded. */
static int sdvo_mode_vrefresh(const struct drm_mode_modeinfo *m)
{
	uint64_t num, den;

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
	return (int)((num + den / 2) / den);
}

static int sdvo_set_output_timings_from_mode(struct sdvo_dev *s, struct sdvo_output *po,
					     const struct drm_mode_modeinfo *mode)
{
	struct sdvo_dtd output_dtd;

	if (!sdvo_set_target_output(s, po->flag))
		return 0;
	sdvo_get_dtd_from_mode(&output_dtd, mode);
	return sdvo_set_output_timing(s, &output_dtd);
}

static int sdvo_create_preferred_input_timing(struct sdvo_dev *s, struct lg_output *o,
					      struct sdvo_output *po,
					      const struct drm_mode_modeinfo *mode)
{
	struct sdvo_preferred_input_timing_args args;

	mm_memset(&args, 0, sizeof(args));
	args.clock = (uint16_t)(mode->clock / 10);
	args.width = mode->hdisplay;
	args.height = mode->vdisplay;

	/* A panel shows anything smaller than itself scaled up. */
	if (SDVO_IS_LVDS(po->flag) && o->fixed_mode_valid &&
	    (o->fixed_mode.hdisplay != args.width || o->fixed_mode.vdisplay != args.height))
		args.flags |= SDVO_PREFERRED_INPUT_TIMING_FLAGS_SCALED;

	return sdvo_set_value(s, SDVO_CMD_CREATE_PREFERRED_INPUT_TIMING, &args, sizeof(args));
}

static int sdvo_get_preferred_input_timing(struct sdvo_dev *s, struct sdvo_dtd *dtd)
{
	return sdvo_get_value(s, SDVO_CMD_GET_PREFERRED_INPUT_TIMING_PART1, &dtd->part1,
			      sizeof(dtd->part1)) &&
	       sdvo_get_value(s, SDVO_CMD_GET_PREFERRED_INPUT_TIMING_PART2, &dtd->part2,
			      sizeof(dtd->part2));
}

/* Ask the chip which input timing it wants for `mode' on the output whose
 * output timing was just set (the chip can only answer once it knows
 * both ends). */
static int sdvo_get_preferred_input_mode(struct sdvo_dev *s, struct lg_output *o,
					 struct sdvo_output *po,
					 const struct drm_mode_modeinfo *mode,
					 struct drm_mode_modeinfo *adjusted)
{
	struct sdvo_dtd input_dtd;

	if (!sdvo_set_target_input(s))
		return 0;
	if (!sdvo_create_preferred_input_timing(s, o, po, mode))
		return 0;
	if (!sdvo_get_preferred_input_timing(s, &input_dtd))
		return 0;

	sdvo_get_mode_from_dtd(adjusted, &input_dtd);
	s->dtd_sdvo_flags = input_dtd.part2.sdvo_flags;
	return 1;
}

/* The link must run between 100 and 200 MHz: slower modes are sent two or
 * four times over and the chip divides the clock back down. */
static int sdvo_get_pixel_multiplier(uint32_t clock)
{
	if (clock >= 100000)
		return 1;
	else if (clock >= 50000)
		return 2;
	else
		return 4;
}

/* A television encoder takes its PLL from the TV clock input with fixed
 * dividers for its clock range, as the video BIOS sets them. */
static int sdvo_adjust_tv_clock(struct lg_config *cfg, uint32_t dotclock)
{
	struct lg_dpll *clock = &cfg->dpll;

	if (dotclock >= 100000 && dotclock < 140500) {
		clock->p1 = 2;
		clock->p2 = 10;
		clock->n = 3;
		clock->m1 = 16;
		clock->m2 = 8;
	} else if (dotclock >= 140500 && dotclock <= 200000) {
		clock->p1 = 1;
		clock->p2 = 10;
		clock->n = 6;
		clock->m1 = 12;
		clock->m2 = 8;
	} else {
		i915_dbg("[drm] i915: SDVO TV clock out of range: %u\n", dotclock);
		return -EINVAL;
	}
	cfg->clock_set = 1;
	return 0;
}

/* ---- HDMI: the AVI infoframe ------------------------------------------------- */

#define SDVO_AVI_INFOFRAME_SIZE 17

/* The CEA-861 video code of a mode (a 59.94 Hz variant matches too), 0
 * when it is none. */
static uint8_t sdvo_cea_vic(const struct drm_mode_modeinfo *m)
{
	struct drm_mode_modeinfo cea;
	uint8_t vic;

	for (vic = 1; vic < 128; vic++) {
		uint32_t alt;

		if (drm_mode_cea_vic(vic, &cea) != 0)
			continue;
		alt = (uint32_t)(((uint64_t)cea.clock * 1000 + 500) / 1001);
		if (m->clock != cea.clock && m->clock != alt)
			continue;
		cea.clock = m->clock;
		if (drm_mode_equal(&cea, m))
			return vic;
	}
	return 0;
}

/* The picture aspect CEA-861 gives its video codes (4:3 = 1, 16:9 = 2). */
static uint8_t sdvo_cea_aspect(uint8_t vic)
{
	static const uint8_t four_three[] = { 1, 2, 6, 7, 10, 11, 14, 15, 17, 21, 22, 25, 26,
					      29, 35, 37, 42, 44, 48, 52, 54, 56, 58 };
	unsigned i;

	if (!vic)
		return 0;
	for (i = 0; i < sizeof(four_three); i++)
		if (four_three[i] == vic)
			return 1;
	return 2;
}

/* AVI infoframe: RGB, the mode's picture aspect, active format = the
 * picture, the RGB quantisation range said out loud (it is always the
 * mode's default here), the video code and the pixel repetition. */
static int sdvo_avi_infoframe(const struct drm_mode_modeinfo *m, int limited, uint8_t *out)
{
	uint8_t vic = sdvo_cea_vic(m);
	uint8_t aspect;
	unsigned sum = 0, i;
	uint8_t *pb = out + 4;

	mm_memset(out, 0, SDVO_AVI_INFOFRAME_SIZE);
	out[0] = 0x82; /* AVI */
	out[1] = 2;
	out[2] = 13;

	switch (m->flags & DRM_MODE_FLAG_PIC_AR_MASK) {
	case DRM_MODE_FLAG_PIC_AR_4_3:
		aspect = 1;
		break;
	case DRM_MODE_FLAG_PIC_AR_16_9:
		aspect = 2;
		break;
	default:
		aspect = sdvo_cea_aspect(vic);
		break;
	}

	pb[0] = (uint8_t)((0u << 5) | (1u << 4)); /* RGB, active format present */
	pb[1] = (uint8_t)((aspect << 4) | 8u); /* active aspect = picture */
	pb[2] = (uint8_t)((limited ? 1u : 2u) << 2);
	pb[3] = vic;
	pb[4] = (m->flags & DRM_MODE_FLAG_DBLCLK) ? 1 : 0;

	for (i = 0; i < SDVO_AVI_INFOFRAME_SIZE; i++)
		sum += out[i];
	out[3] = (uint8_t)(0x100 - (sum & 0xff));
	return SDVO_AVI_INFOFRAME_SIZE;
}

static int sdvo_get_hbuf_size(struct sdvo_dev *s, uint8_t *hbuf_size)
{
	if (!sdvo_get_value(s, SDVO_CMD_GET_HBUF_INFO, hbuf_size, 1))
		return 0;
	/* The size is 0-based, except that 0 means none. */
	if (*hbuf_size)
		(*hbuf_size)++;
	return 1;
}

static int sdvo_write_infoframe(struct sdvo_dev *s, unsigned if_index, uint8_t tx_rate,
				const uint8_t *data, unsigned length)
{
	uint8_t set_buf_index[2] = { (uint8_t)if_index, 0 };
	uint8_t hbuf_size, tmp[8];
	unsigned i;

	if (!sdvo_set_value(s, SDVO_CMD_SET_HBUF_INDEX, set_buf_index, 2))
		return 0;
	if (!sdvo_get_hbuf_size(s, &hbuf_size))
		return 0;

	i915_dbg("[drm] i915: SDVO%c: writing hbuf %u, length %u, hbuf_size %u\n",
		 sdvo_port_name(s), if_index, length, hbuf_size);

	if (hbuf_size < length)
		return 0;

	for (i = 0; i < hbuf_size; i += 8) {
		mm_memset(tmp, 0, 8);
		if (i < length)
			mm_memcpy(tmp, data + i, length - i < 8 ? length - i : 8);
		if (!sdvo_set_value(s, SDVO_CMD_SET_HBUF_DATA, tmp, 8))
			return 0;
	}

	return sdvo_set_value(s, SDVO_CMD_SET_HBUF_TXRATE, &tx_rate, 1);
}

/* ---- television formats ----------------------------------------------------- */

static int sdvo_set_tv_format(struct sdvo_dev *s, struct sdvo_output *po)
{
	uint8_t format[6];
	uint32_t format_map = 1u << po->tv_format;

	mm_memset(format, 0, sizeof(format));
	format[0] = (uint8_t)format_map;
	format[1] = (uint8_t)(format_map >> 8);
	format[2] = (uint8_t)(format_map >> 16);
	format[3] = (uint8_t)(format_map >> 24);
	return sdvo_set_value(s, SDVO_CMD_SET_TV_FORMAT, format, 6);
}

/* ---- the DDC proxy ----------------------------------------------------------- */

#define SDVO_PROXY_LOCAL_MSGS 12

/* Reach the monitor behind the chip: the bus switch command opens the
 * chosen DDC bus for the transfer that follows it, so it travels in the
 * same transfer as the DDC messages, with nothing in between. */
static int sdvo_ddc_proxy_xfer(struct i2c_adapter *a, struct i2c_msg *msgs, int n)
{
	struct sdvo_ddc *ddc = a->priv;
	struct sdvo_dev *s = ddc->dev;
	struct i2c_msg local[SDVO_PROXY_LOCAL_MSGS];
	struct i2c_msg *all = local;
	uint8_t buf[4], status, bus = (uint8_t)(1u << ddc->bus);
	int pre, ret, i;

	if (n <= 0)
		return 0;
	if (n + 4 > SDVO_PROXY_LOCAL_MSGS) {
		all = kalloc(sizeof(*all) * (size_t)(n + 4));
		if (!all)
			return -ENOMEM;
	}

	sdvo_debug_write(s, SDVO_CMD_SET_CONTROL_BUS_SWITCH, &bus, 1);
	pre = sdvo_build_cmd(s, SDVO_CMD_SET_CONTROL_BUS_SWITCH, &bus, 1, all, buf, &status);
	for (i = 0; i < n; i++)
		all[pre + i] = msgs[i];

	ret = i2c_transfer(s->i2c, all, pre + n);
	if (all != local)
		kfree(all);
	if (ret < 0)
		return ret;
	if (ret < pre)
		return -EIO;
	return ret - pre;
}

static void sdvo_init_ddc_proxy(struct sdvo_ddc *ddc, struct sdvo_dev *s, int ddc_bus)
{
	ddc->dev = s;
	ddc->bus = (uint8_t)ddc_bus;
	ddc->adap.xfer = sdvo_ddc_proxy_xfer;
	ddc->adap.priv = ddc;
	ksnprintf(ddc->adap.name, sizeof(ddc->adap.name), "SDVO %c DDC%d", sdvo_port_name(s),
		  ddc_bus);
}

/* The DDC bus of an output when the VBT does not say: buses are numbered
 * in the order RGB, TMDS, LVDS over the outputs the chip has. */
static int sdvo_guess_ddc_bus(struct sdvo_dev *s, uint16_t flag)
{
	uint16_t mask = 0;
	int num_bits = 0;
	unsigned i;

	switch (flag) {
	case SDVO_OUTPUT_LVDS1:
		mask |= SDVO_OUTPUT_LVDS1;
		/* fall through */
	case SDVO_OUTPUT_LVDS0:
		mask |= SDVO_OUTPUT_LVDS0;
		/* fall through */
	case SDVO_OUTPUT_TMDS1:
		mask |= SDVO_OUTPUT_TMDS1;
		/* fall through */
	case SDVO_OUTPUT_TMDS0:
		mask |= SDVO_OUTPUT_TMDS0;
		/* fall through */
	case SDVO_OUTPUT_RGB1:
		mask |= SDVO_OUTPUT_RGB1;
		/* fall through */
	case SDVO_OUTPUT_RGB0:
		mask |= SDVO_OUTPUT_RGB0;
		break;
	default:
		break;
	}

	mask &= s->caps.output_flags;
	for (i = 0; i < 16; i++)
		if (mask & (1u << i))
			num_bits++;
	/* more than three outputs: bus 3 for the rest */
	if (num_bits > 3)
		num_bits = 3;
	return num_bits;
}

static struct i2c_adapter *sdvo_select_ddc_bus(struct sdvo_dev *s, uint16_t flag)
{
	struct lg_display *d = s->d;
	int idx = s->port - LG_PORT_B;
	int ddc_bus;

	if (d->vbt.sdvo[idx].initialized)
		ddc_bus = (d->vbt.sdvo[idx].ddc_pin & 0xf0) >> 4;
	else
		ddc_bus = sdvo_guess_ddc_bus(s, flag);

	if (ddc_bus < 1 || ddc_bus > 3)
		return NULL;
	return &s->ddc[ddc_bus - 1].adap;
}

/* ---- the control bus ---------------------------------------------------------- */

static uint8_t sdvo_get_target_addr(struct lg_display *d, int port)
{
	int mine = port == LG_PORT_B ? 0 : 1;
	uint8_t my_addr = d->vbt.sdvo[mine].slave_addr;
	uint8_t other_addr = d->vbt.sdvo[!mine].slave_addr;

	/* The VBT described this encoder. */
	if (my_addr)
		return my_addr;
	/* It described only the other one: take the address it does not use. */
	if (other_addr)
		return other_addr == 0x70 ? 0x72 : 0x70;
	/* Neither: B answers at 0x70, C at 0x72. */
	return port == LG_PORT_B ? 0x70 : 0x72;
}

static void sdvo_select_i2c_bus(struct sdvo_dev *s)
{
	struct lg_display *d = s->d;
	int idx = s->port - LG_PORT_B;
	uint8_t pin;

	if (d->vbt.sdvo[idx].initialized && lg_gmbus_pin_valid(d, d->vbt.sdvo[idx].i2c_pin))
		pin = d->vbt.sdvo[idx].i2c_pin;
	else
		pin = LG_GMBUS_PIN_DPB;

	i915_dbg("[drm] i915: SDVO%c: I2C pin %u, target addr 0x%x\n", sdvo_port_name(s), pin,
		 s->addr);

	s->i2c_pin = pin;
	s->i2c = lg_gmbus_adapter(d, pin);
	lg_gmbus_set_speed(d, pin, GMBUS_RATE_1MHZ);
	/* The GMBUS controller should drive the control bus at full speed,
	 * but the chips fail on it: bit-bang the pin instead. */
	lg_gmbus_force_bit(d, pin, 1);
}

/* Undo sdvo_select_i2c_bus, unless the other encoder shares the pin. */
static void sdvo_unselect_i2c_bus(struct sdvo_dev *s)
{
	int i;

	for (i = 0; i < 2; i++)
		if (g_sdvo[i] && g_sdvo[i] != s && g_sdvo[i]->i2c_pin == s->i2c_pin)
			return;
	lg_gmbus_force_bit(s->d, s->i2c_pin, 0);
	lg_gmbus_set_speed(s->d, s->i2c_pin, GMBUS_RATE_100KHZ);
}

/* ---- EDID --------------------------------------------------------------------- */

/* Read the sink's EDID into o->edid over adapter `a'. */
static int sdvo_read_edid_on(struct lg_display *d, struct lg_output *o, struct i2c_adapter *a)
{
	struct i2c_adapter *save = o->ddc;
	int ret;

	if (!a)
		return -ENODEV;
	o->ddc = a;
	ret = lg_read_edid(d, o);
	o->ddc = save;
	if (ret)
		o->edid_len = 0;
	return ret;
}

/* The output's own DDC bus, else -- a DVI-I connector whose analog and
 * digital halves share one DDC line, as on the Mac mini -- the analog
 * port's bus. */
static int sdvo_get_edid(struct lg_display *d, struct lg_output *o)
{
	int ret = -ENODEV;

	if (o->ddc)
		ret = sdvo_read_edid_on(d, o, o->ddc);
	if (ret)
		ret = sdvo_read_edid_on(d, o, lg_gmbus_adapter(d, d->vbt.crt_ddc_pin));
	return ret;
}

static int sdvo_edid_is_digital(const struct lg_output *o)
{
	return o->edid_len >= DRM_EDID_BLOCK && (o->edid[20] & 0x80);
}

static void sdvo_forget_edid(struct lg_output *o)
{
	o->edid_len = 0;
	o->hdmi_sink = 0;
	o->has_audio = 0;
}

/* A shared DDC bus may carry another connector's monitor: the EDID must
 * be of the output's kind. */
static int sdvo_edid_matches(const struct lg_output *o, const struct sdvo_output *po)
{
	int monitor_is_digital = sdvo_edid_is_digital(o);
	int connector_is_digital = SDVO_IS_DIGITAL(po->flag);

	i915_dbg("[drm] i915: %s: connector_is_digital? %d, monitor_is_digital? %d\n", o->name,
		 connector_is_digital, monitor_is_digital);
	return connector_is_digital == monitor_is_digital;
}

/* ---- output hooks --------------------------------------------------------------- */

static int sdvo_detect(struct lg_display *d, struct lg_output *o)
{
	struct sdvo_output *po = o->priv;
	struct sdvo_dev *s = po->dev;
	uint16_t response;
	int connected;

	/* A panel is always there. */
	if (SDVO_IS_LVDS(po->flag)) {
		o->detected = 1;
		return 1;
	}

	if (!sdvo_set_target_output(s, po->flag))
		goto off;
	if (!sdvo_get_value(s, SDVO_CMD_GET_ATTACHED_DISPLAYS, &response, 2))
		goto off;

	i915_dbg("[drm] i915: %s: SDVO response %u %u [%x]\n", o->name, response & 0xff,
		 response >> 8, po->flag);

	if (response == 0 || (po->flag & response) == 0)
		goto off;

	if (SDVO_IS_TMDS(po->flag)) {
		/* Without an EDID the state is unknown: not usable. */
		if (sdvo_get_edid(d, o) != 0)
			goto off;
		if (!sdvo_edid_is_digital(o))
			goto off;
		connected = 1;
	} else {
		if (sdvo_get_edid(d, o) == 0) {
			connected = sdvo_edid_matches(o, po);
			if (!connected || !SDVO_HAS_DDC(po->flag))
				sdvo_forget_edid(o);
		} else {
			connected = 1;
		}
	}
	o->detected = connected;
	return connected;

off:
	sdvo_forget_edid(o);
	o->detected = 0;
	return 0;
}

static uint32_t sdvo_max_clock(struct lg_display *d, struct sdvo_dev *s)
{
	uint32_t max = (uint32_t)s->pixel_clock_max;

	if (d->max_dotclk_khz && d->max_dotclk_khz < max)
		max = d->max_dotclk_khz;
	return max;
}

static int sdvo_get_tv_modes(struct lg_display *d, struct lg_output *o, int conn)
{
	struct sdvo_output *po = o->priv;
	struct sdvo_dev *s = po->dev;
	uint32_t format_map = 1u << po->tv_format;
	uint8_t tv_res[3], reply[3];
	uint32_t res;
	int i, n = 0;

	tv_res[0] = (uint8_t)format_map;
	tv_res[1] = (uint8_t)(format_map >> 8);
	tv_res[2] = (uint8_t)(format_map >> 16);

	if (!sdvo_set_target_output(s, po->flag))
		return 0;
	if (!sdvo_write_cmd(s, SDVO_CMD_GET_SDTV_RESOLUTION_SUPPORT, tv_res, 3))
		return 0;
	if (!sdvo_read_response(s, reply, 3))
		return 0;
	res = (uint32_t)reply[0] | ((uint32_t)reply[1] << 8) | ((uint32_t)reply[2] << 16);

	for (i = 0; i < SDVO_TV_MODES_NUM; i++) {
		struct drm_mode_modeinfo m;

		if (!(res & (1u << i)))
			continue;
		mm_memset(&m, 0, sizeof(m));
		m.clock = sdvo_tv_modes[i].clock;
		m.hdisplay = sdvo_tv_modes[i].hd;
		m.hsync_start = sdvo_tv_modes[i].hss;
		m.hsync_end = sdvo_tv_modes[i].hse;
		m.htotal = sdvo_tv_modes[i].ht;
		m.vdisplay = sdvo_tv_modes[i].vd;
		m.vsync_start = sdvo_tv_modes[i].vss;
		m.vsync_end = sdvo_tv_modes[i].vse;
		m.vtotal = sdvo_tv_modes[i].vt;
		m.flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC;
		m.type = DRM_MODE_TYPE_DRIVER;
		drm_mode_finish(&m);
		if (drm_connector_add_mode(d->drm, conn, &m) == 0)
			n++;
	}
	return n;
}

/* A panel: its own timing, and the standard modes it can be scaled up
 * from (smaller, at the panel's refresh rate). */
static int sdvo_get_lvds_modes(struct lg_display *d, struct lg_output *o, int conn)
{
	struct sdvo_output *po = o->priv;
	struct drm_mode_modeinfo m;
	int vrefresh, i, n = 0;

	if (!o->fixed_mode_valid)
		return 0;

	m = o->fixed_mode;
	m.type |= DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER;
	if (drm_connector_add_mode(d->drm, conn, &m) == 0)
		n++;

	vrefresh = sdvo_mode_vrefresh(&o->fixed_mode);
	for (i = 0; drm_mode_table_get(i, &m) == 0; i++) {
		int r = sdvo_mode_vrefresh(&m);

		if (m.hdisplay > o->fixed_mode.hdisplay || m.vdisplay > o->fixed_mode.vdisplay)
			continue;
		if (m.hdisplay == o->fixed_mode.hdisplay && m.vdisplay == o->fixed_mode.vdisplay)
			continue;
		if (m.flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLSCAN))
			continue;
		if (r - vrefresh > 1 || vrefresh - r > 1)
			continue;
		if ((int)m.clock < po->dev->pixel_clock_min || (int)m.clock > po->dev->pixel_clock_max)
			continue;
		if (drm_connector_add_mode(d->drm, conn, &m) == 0)
			n++;
	}
	return n;
}

static int sdvo_get_modes(struct lg_display *d, struct lg_output *o, int conn)
{
	struct sdvo_output *po = o->priv;

	if (SDVO_IS_TV(po->flag))
		return sdvo_get_tv_modes(d, o, conn);
	if (SDVO_IS_LVDS(po->flag))
		return sdvo_get_lvds_modes(d, o, conn);

	/* Through the bus switch, or the analog port's bus on a shared DDC. */
	if (o->edid_len == 0)
		sdvo_get_edid(d, o);
	if (o->edid_len && !sdvo_edid_matches(o, po)) {
		sdvo_forget_edid(o);
		return 0;
	}
	return lg_get_modes_edid(d, o, conn, sdvo_max_clock(d, po->dev));
}

static int sdvo_mode_valid(struct lg_display *d, struct lg_output *o,
			   const struct drm_mode_modeinfo *m)
{
	struct sdvo_output *po = o->priv;
	struct sdvo_dev *s = po->dev;
	int has_hdmi_sink = po->is_hdmi && o->hdmi_sink;
	int clock = (int)m->clock;

	if (m->flags & DRM_MODE_FLAG_DBLSCAN)
		return -EINVAL;
	if (d->max_dotclk_khz && m->clock > d->max_dotclk_khz)
		return -EINVAL;

	if (m->flags & DRM_MODE_FLAG_DBLCLK) {
		if (!has_hdmi_sink)
			return -EINVAL;
		clock *= 2;
	}

	if (s->pixel_clock_min > clock)
		return -EINVAL;
	if (s->pixel_clock_max < clock)
		return -EINVAL;

	if (SDVO_IS_LVDS(po->flag) && o->fixed_mode_valid) {
		int r = sdvo_mode_vrefresh(m), fr = sdvo_mode_vrefresh(&o->fixed_mode);

		if (m->hdisplay > o->fixed_mode.hdisplay)
			return -EINVAL;
		if (m->vdisplay > o->fixed_mode.vdisplay)
			return -EINVAL;
		if (r - fr > 1 || fr - r > 1)
			return -EINVAL;
		if (d->max_dotclk_khz && o->fixed_mode.clock > d->max_dotclk_khz)
			return -EINVAL;
	}
	return 0;
}

static int sdvo_compute_config(struct lg_display *d, struct lg_output *o, struct lg_config *cfg)
{
	struct sdvo_output *po = o->priv;
	struct sdvo_dev *s = po->dev;
	struct drm_mode_modeinfo adjusted = cfg->mode;
	int vic, ret;

	po->adjusted_valid = 0;

	if (d->pch != LG_PCH_NONE)
		cfg->has_pch_encoder = 1;

	/* 8 bits per colour into the chip */
	cfg->pipe_bpp = 24;

	/* The input timing is built from the output timing, so the output
	 * timing has to be set already now. */
	if (SDVO_IS_TV(po->flag)) {
		if (!sdvo_set_output_timings_from_mode(s, po, &cfg->mode))
			return -EINVAL;
		(void)sdvo_get_preferred_input_mode(s, o, po, &cfg->mode, &adjusted);
		cfg->sdvo_tv_clock = 1;
	} else if (SDVO_IS_LVDS(po->flag)) {
		int r, fr;

		if (!o->fixed_mode_valid)
			return -EINVAL;
		/* Do not lie much about the refresh rate the client gets. */
		r = sdvo_mode_vrefresh(&cfg->mode);
		fr = sdvo_mode_vrefresh(&o->fixed_mode);
		if (r - fr > 1 || fr - r > 1) {
			i915_dbg("[drm] i915: %s: requested %d Hz, the panel runs %d Hz\n", o->name,
				 r, fr);
			return -EINVAL;
		}
		adjusted = o->fixed_mode;

		if (!sdvo_set_output_timings_from_mode(s, po, &o->fixed_mode))
			return -EINVAL;
		if (!sdvo_get_preferred_input_mode(s, o, po, &cfg->mode, &adjusted) &&
		    (cfg->mode.hdisplay != o->fixed_mode.hdisplay ||
		     cfg->mode.vdisplay != o->fixed_mode.vdisplay)) {
			/* The chip gave no scaled input timing; the pipe
			 * fitter is not used for SDVO. */
			return -EINVAL;
		}
	}

	if (adjusted.flags & DRM_MODE_FLAG_DBLSCAN)
		return -EINVAL;

	/* The PLL runs at the multiplied clock; the chip divides it out. */
	cfg->pixel_multiplier = sdvo_get_pixel_multiplier(adjusted.clock);

	cfg->has_hdmi_sink = po->is_hdmi && o->hdmi_sink;
	cfg->has_infoframe = cfg->has_hdmi_sink;
	cfg->has_audio = 0;

	/* CEA-861 modes other than 640x480 default to limited range, when
	 * the chip can send it. */
	vic = sdvo_cea_vic(&adjusted);
	cfg->limited_color_range = (s->colorimetry_cap & SDVO_COLORIMETRY_RGB220) &&
				   cfg->has_hdmi_sink && vic > 1;

	/* after the pixel multiplier */
	if (SDVO_IS_TV(po->flag)) {
		ret = sdvo_adjust_tv_clock(cfg, adjusted.clock);
		if (ret)
			return ret;
	}

	cfg->port_clock = adjusted.clock * (uint32_t)cfg->pixel_multiplier;

	if (SDVO_IS_TV(po->flag) || SDVO_IS_LVDS(po->flag)) {
		/* The pipe runs the input timing the chip asked for. */
		lg_timings_from_mode(&cfg->t, &adjusted);
		cfg->t_set = 1;
		cfg->pixel_rate = adjusted.clock;
		po->adjusted = adjusted;
		po->adjusted_valid = 1;
	}
	return 0;
}

static void sdvo_pre_enable(struct lg_display *d, struct lg_output *o, const struct lg_config *cfg)
{
	struct sdvo_output *po = o->priv;
	struct sdvo_dev *s = po->dev;
	struct drm_mode_modeinfo adjusted;
	struct sdvo_in_out_map in_out;
	struct sdvo_dtd input_dtd, output_dtd;
	uint32_t sdvox;
	uint8_t rate;

	if (cfg->t_set && po->adjusted_valid)
		adjusted = po->adjusted;
	else
		adjusted = cfg->mode;

	/* Map input 0 to this output.  Right for a single-input chip, whose
	 * input 0 is the port; a two-input chip has B on 0 and C on 1. */
	in_out.in0 = po->flag;
	in_out.in1 = 0;
	sdvo_set_value(s, SDVO_CMD_SET_IN_OUT_MAP, &in_out, sizeof(in_out));

	/* the output timing to the screen */
	if (!sdvo_set_target_output(s, po->flag))
		return;
	if (SDVO_IS_LVDS(po->flag) && o->fixed_mode_valid)
		sdvo_get_dtd_from_mode(&output_dtd, &o->fixed_mode);
	else
		sdvo_get_dtd_from_mode(&output_dtd, &cfg->mode);
	if (!sdvo_set_output_timing(s, &output_dtd))
		kprintf("[drm] i915: %s: setting output timings failed\n", o->name);

	/* the input timing, on input 0 */
	if (!sdvo_set_target_input(s))
		return;

	if (cfg->has_hdmi_sink) {
		uint8_t avi[SDVO_AVI_INFOFRAME_SIZE];
		int len;

		sdvo_set_encode(s, SDVO_ENCODE_HDMI);
		sdvo_set_colorimetry(s, cfg->limited_color_range ? SDVO_COLORIMETRY_RGB220 :
								    SDVO_COLORIMETRY_RGB256);
		if (cfg->has_infoframe) {
			len = sdvo_avi_infoframe(&adjusted, cfg->limited_color_range, avi);
			sdvo_write_infoframe(s, SDVO_HBUF_INDEX_AVI_IF, SDVO_HBUF_TX_VSYNC, avi,
					     (unsigned)len);
		}
		sdvo_set_pixel_replication(s, (adjusted.flags & DRM_MODE_FLAG_DBLCLK) ? 1 : 0);
	} else {
		sdvo_set_encode(s, SDVO_ENCODE_DVI);
	}

	if (SDVO_IS_TV(po->flag) && !sdvo_set_tv_format(s, po))
		return;

	sdvo_get_dtd_from_mode(&input_dtd, &adjusted);
	if (SDVO_IS_TV(po->flag) || SDVO_IS_LVDS(po->flag))
		input_dtd.part2.sdvo_flags = s->dtd_sdvo_flags;
	if (!sdvo_set_input_timing(s, &input_dtd))
		kprintf("[drm] i915: %s: setting input timings failed\n", o->name);

	switch (cfg->pixel_multiplier) {
	default:
		kprintf("[drm] i915: %s: unknown pixel multiplier %d\n", o->name,
			cfg->pixel_multiplier);
		/* fall through */
	case 1:
		rate = SDVO_CLOCK_RATE_MULT_1X;
		break;
	case 2:
		rate = SDVO_CLOCK_RATE_MULT_2X;
		break;
	case 4:
		rate = SDVO_CLOCK_RATE_MULT_4X;
		break;
	}
	if (!sdvo_set_clock_rate_mult(s, rate))
		return;

	/* the port register */
	if (d->ver >= 4) {
		/* The sync polarity the sink sees comes from the timing sent
		 * to the chip; the port runs both high. */
		sdvox = SDVO_VSYNC_ACTIVE_HIGH | SDVO_HSYNC_ACTIVE_HIGH;
		if (d->ver < 5)
			sdvox |= SDVO_BORDER_ENABLE;
	} else {
		sdvox = lg_rd(d, s->reg);
		if (s->port == LG_PORT_B)
			sdvox &= SDVOB_PRESERVE_MASK;
		else
			sdvox &= SDVOC_PRESERVE_MASK;
		sdvox |= (9u << 19) | SDVO_BORDER_ENABLE;
	}

	if (d->pch == LG_PCH_CPT)
		sdvox |= SDVO_PIPE_SEL_CPT(cfg->pipe);
	else
		sdvox |= SDVO_PIPE_SEL(cfg->pipe);

	/* The multiplier: gen4 on keeps it in DPLL_MD, the i945G/GM, G33 and
	 * Pineview in the DPLL (the PLL code writes both); only the
	 * i915G/GM have it in the port. */
	if (d->ver < 4 && !(d->is_i945g || d->is_i945gm || d->is_g33 || d->is_pnv))
		sdvox |= (uint32_t)(cfg->pixel_multiplier - 1) << SDVO_PORT_MULTIPLY_SHIFT;

	if ((input_dtd.part2.sdvo_flags & SDVO_NEED_TO_STALL) && d->ver < 5)
		sdvox |= SDVO_STALL_SELECT;

	sdvo_write_sdvox(s, sdvox);
}

static void sdvo_enable(struct lg_display *d, struct lg_output *o, const struct lg_config *cfg)
{
	struct sdvo_output *po = o->priv;
	struct sdvo_dev *s = po->dev;
	int input1 = 0, input2 = 0, i, success;

	sdvo_write_sdvox(s, lg_rd(d, s->reg) | SDVO_ENABLE);

	for (i = 0; i < 2; i++)
		lg_wait_for_vblank(d, cfg->pipe);

	/* Many chips never report sync; a successful status is enough. */
	success = sdvo_get_trained_inputs(s, &input1, &input2);
	if (success && !input1)
		i915_dbg("[drm] i915: %s: first SDVO input reported failure to sync\n", o->name);

	sdvo_set_active_outputs(s, po->flag);
}

static void sdvo_port_disable(struct lg_display *d, struct lg_output *o,
			      const struct lg_config *cfg)
{
	struct sdvo_output *po = o->priv;
	struct sdvo_dev *s = po->dev;
	uint32_t temp;

	sdvo_set_active_outputs(s, 0);

	temp = lg_rd(d, s->reg);
	temp &= ~SDVO_ENABLE;
	sdvo_write_sdvox(s, temp);

	/* Ibex Peak: a port left selecting transcoder B keeps the matching
	 * DP port from being enabled on transcoder A; move it to A after
	 * disabling.  The other pipe sees FIFO underruns meanwhile. */
	if (d->pch == LG_PCH_IBX && cfg->pipe == 1) {
		int reported = d->pipes[0].underrun_reported;

		d->pipes[0].underrun_reported = 1;

		temp &= ~SDVO_PIPE_SEL_MASK;
		temp |= SDVO_ENABLE | SDVO_PIPE_SEL(0);
		sdvo_write_sdvox(s, temp);

		temp &= ~SDVO_ENABLE;
		sdvo_write_sdvox(s, temp);

		if (d->pipes[0].active)
			lg_wait_for_vblank(d, 0);
		d->pipes[0].underrun_reported = reported;
	}
}

/* GMCH: the port goes off before the pipe; with a PCH after it. */
static void sdvo_disable(struct lg_display *d, struct lg_output *o, const struct lg_config *cfg)
{
	if (d->pch == LG_PCH_NONE)
		sdvo_port_disable(d, o, cfg);
}

static void sdvo_post_disable(struct lg_display *d, struct lg_output *o,
			      const struct lg_config *cfg)
{
	if (d->pch != LG_PCH_NONE)
		sdvo_port_disable(d, o, cfg);
}

static int sdvo_get_hw_state(struct lg_display *d, struct lg_output *o, int *pipe)
{
	struct sdvo_output *po = o->priv;
	struct sdvo_dev *s = po->dev;
	uint16_t active_outputs = 0;
	uint32_t val = lg_rd(d, s->reg);
	int port_on = (val & SDVO_ENABLE) != 0;

	if (d->pch == LG_PCH_CPT)
		*pipe = (int)((val & SDVO_PIPE_SEL_MASK_CPT) >> SDVO_PIPE_SEL_SHIFT_CPT);
	else
		*pipe = (int)((val & SDVO_PIPE_SEL_MASK) >> SDVO_PIPE_SEL_SHIFT);

	if (!sdvo_get_active_outputs(s, &active_outputs)) {
		/* No answer: the port alone says, for the chip's first
		 * output only. */
		return port_on && (int)(o - d->out) == s->first_out;
	}
	return port_on && (active_outputs & po->flag) != 0;
}

static void sdvo_hpd_event(struct lg_display *d, struct lg_output *o, int long_pulse)
{
	struct sdvo_output *po = o->priv;

	(void)d;
	(void)long_pulse;
	sdvo_enable_hotplug(po->dev);
}

/* After a sleep the chip has forgotten which hotplug interrupts it
 * should raise. */
static void sdvo_resume(struct lg_display *d, struct lg_output *o)
{
	struct sdvo_output *po = o->priv;
	struct sdvo_dev *s = po->dev;

	if (s->hotplug_active && (int)(o - d->out) == s->first_out)
		sdvo_enable_hotplug(s);
}

static const struct lg_output_funcs sdvo_funcs = {
	.detect = sdvo_detect,
	.get_modes = sdvo_get_modes,
	.mode_valid = sdvo_mode_valid,
	.compute_config = sdvo_compute_config,
	.pre_enable = sdvo_pre_enable,
	.enable = sdvo_enable,
	.disable = sdvo_disable,
	.post_disable = sdvo_post_disable,
	.get_hw_state = sdvo_get_hw_state,
	.hpd_event = sdvo_hpd_event,
	.resume = sdvo_resume,
};

/* ---- probing ------------------------------------------------------------------ */

static void sdvo_output_free(struct sdvo_output *po)
{
	if (!po)
		return;
	if (po->edid)
		kfree(po->edid);
	kfree(po);
}

/* Read an EDID over `a' into po (kept for the output). */
static int sdvo_probe_edid(struct sdvo_output *po, struct i2c_adapter *a, struct drm_edid_info *info)
{
	int blocks;

	if (!a)
		return -ENODEV;
	if (!po->edid) {
		po->edid = kalloc(DRM_EDID_BLOCK * DRM_EDID_MAX_BLOCKS);
		if (!po->edid)
			return -ENOMEM;
	}
	blocks = drm_edid_read(a, po->edid, DRM_EDID_MAX_BLOCKS);
	if (blocks <= 0) {
		po->edid_len = 0;
		return -ENODEV;
	}
	po->edid_len = blocks * DRM_EDID_BLOCK;
	if (drm_edid_parse(po->edid, (unsigned)po->edid_len, info) != 0) {
		po->edid_len = 0;
		return -EINVAL;
	}
	return 0;
}

/* Everything one output function needs, asked of the chip; NULL when the
 * function cannot be used (which fails the whole chip, as an incomplete
 * set of outputs would be misleading). */
static struct sdvo_output *sdvo_output_prepare(struct sdvo_dev *s, uint16_t type)
{
	struct lg_display *d = s->d;
	struct sdvo_output *po;
	struct drm_edid_info *info = NULL;

	po = kalloc(sizeof(*po));
	if (!po)
		return NULL;
	mm_memset(po, 0, sizeof(*po));
	po->dev = s;
	po->flag = type;
	if (SDVO_HAS_DDC(type))
		po->ddc = sdvo_select_ddc_bus(s, type);

	if (type & SDVO_TMDS_MASK) {
		i915_dbg("[drm] i915: SDVO%c: initialising DVI type 0x%x\n", sdvo_port_name(s), type);
		po->kind = type == SDVO_OUTPUT_TMDS0 ? "TMDS" : "TMDS1";
		if (sdvo_get_hotplug_support(s) & type) {
			s->hotplug_active |= type;
			po->hotplug = 1;
			sdvo_enable_hotplug(s);
		}
		po->enc_type = DRM_MODE_ENCODER_TMDS;
		po->conn_type = DRM_MODE_CONNECTOR_DVID;
		if (sdvo_check_supp_encode(s)) {
			po->is_hdmi = 1;
			info = kalloc(sizeof(*info));
			if (info && sdvo_probe_edid(po, po->ddc, info) == 0 && info->is_hdmi)
				po->conn_type = DRM_MODE_CONNECTOR_HDMIA;
		}
	} else if (type & SDVO_TV_MASK) {
		uint8_t format[6];
		uint32_t format_map;
		int i;

		i915_dbg("[drm] i915: SDVO%c: initialising TV type 0x%x\n", sdvo_port_name(s), type);
		po->enc_type = DRM_MODE_ENCODER_TVDAC;
		if (type == SDVO_OUTPUT_SVID0) {
			po->kind = "SVID";
			po->conn_type = DRM_MODE_CONNECTOR_SVIDEO;
		} else if (type == SDVO_OUTPUT_CVBS0) {
			po->kind = "CVBS";
			po->conn_type = DRM_MODE_CONNECTOR_Composite;
		} else {
			po->kind = "YPRPB";
			po->conn_type = DRM_MODE_CONNECTOR_Component;
		}
		/* The standards it supports; the first is used. */
		if (!sdvo_set_target_output(s, type))
			goto fail;
		if (!sdvo_get_value(s, SDVO_CMD_GET_SUPPORTED_TV_FORMATS, format, 6))
			goto fail;
		format_map = (uint32_t)format[0] | ((uint32_t)format[1] << 8) |
			     ((uint32_t)format[2] << 16) | ((uint32_t)format[3] << 24);
		po->tv_format = -1;
		for (i = 0; i < SDVO_TV_FORMAT_NUM; i++) {
			if (format_map & (1u << i)) {
				po->tv_format = i;
				break;
			}
		}
		if (po->tv_format < 0)
			goto fail;
		i915_dbg("[drm] i915: SDVO%c: TV formats 0x%x, using %s\n", sdvo_port_name(s),
			 format_map, sdvo_tv_format_names[po->tv_format]);
	} else if (type & SDVO_RGB_MASK) {
		i915_dbg("[drm] i915: SDVO%c: initialising analog type 0x%x\n", sdvo_port_name(s),
			 type);
		po->kind = type == SDVO_OUTPUT_RGB0 ? "RGB" : "RGB1";
		po->enc_type = DRM_MODE_ENCODER_DAC;
		po->conn_type = DRM_MODE_CONNECTOR_VGA;
	} else if (type & SDVO_LVDS_MASK) {
		i915_dbg("[drm] i915: SDVO%c: initialising LVDS type 0x%x\n", sdvo_port_name(s),
			 type);
		po->kind = type == SDVO_OUTPUT_LVDS0 ? "LVDS" : "LVDS1";
		po->enc_type = DRM_MODE_ENCODER_LVDS;
		po->conn_type = DRM_MODE_CONNECTOR_LVDS;
		/* The VBT's mode first: some SDVO-to-LVDS chips cannot cope
		 * with the panel's EDID mode. */
		if (d->vbt.sdvo_lvds_mode_valid) {
			po->fixed = d->vbt.sdvo_lvds_mode;
			po->fixed_valid = 1;
			po->mm_w = d->vbt.lfp_width_mm;
			po->mm_h = d->vbt.lfp_height_mm;
		}
		info = kalloc(sizeof(*info));
		if (info && sdvo_probe_edid(po, po->ddc, info) == 0) {
			if (!po->fixed_valid && info->nmodes > 0) {
				int pref = info->preferred >= 0 ? info->preferred : 0;

				po->fixed = info->modes[pref];
				po->fixed_valid = 1;
			}
			if (info->mm_width && info->mm_height) {
				po->mm_w = info->mm_width;
				po->mm_h = info->mm_height;
			}
		}
		if (!po->fixed_valid)
			goto fail;
		po->fixed.type |= DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER;
		drm_mode_finish(&po->fixed);
	} else {
		goto fail;
	}

	if (info)
		kfree(info);
	return po;

fail:
	if (info)
		kfree(info);
	sdvo_output_free(po);
	return NULL;
}

/* An XXX1 function exists only next to its XXX0. */
static uint16_t sdvo_filter_output_flags(uint16_t flags)
{
	flags &= SDVO_OUTPUT_MASK;
	if (!(flags & SDVO_OUTPUT_TMDS0))
		flags &= (uint16_t)~SDVO_OUTPUT_TMDS1;
	if (!(flags & SDVO_OUTPUT_RGB0))
		flags &= (uint16_t)~SDVO_OUTPUT_RGB1;
	if (!(flags & SDVO_OUTPUT_LVDS0))
		flags &= (uint16_t)~SDVO_OUTPUT_LVDS1;
	return flags;
}

static void sdvo_output_create(struct lg_display *d, struct sdvo_dev *s, struct sdvo_output *po,
			       struct lg_output *o)
{
	o->type = LG_OUTPUT_SDVO;
	o->port = s->port;
	o->reg = s->reg;
	ksnprintf(o->name, sizeof(o->name), "SDVO-%c-%s", sdvo_port_name(s), po->kind);
	o->funcs = &sdvo_funcs;
	o->conn_type = po->conn_type;
	o->enc_type = po->enc_type;
	/* Every pipe; cloning an SDVO encoder with anything never works. */
	o->pipe_mask = (1u << d->num_pipes) - 1;
	if (po->hotplug) {
		o->hpd_pin = s->port == LG_PORT_B ? LG_HPD_SDVO_B : LG_HPD_SDVO_C;
	} else {
		o->hpd_pin = LG_HPD_NONE;
		o->polled = 1;
	}
	o->ddc_pin = s->i2c_pin;
	o->ddc = po->ddc;
	if (po->edid && po->edid_len > 0 && po->edid_len <= (int)sizeof(o->edid)) {
		struct drm_edid_info *info = kalloc(sizeof(*info));

		mm_memcpy(o->edid, po->edid, (size_t)po->edid_len);
		o->edid_len = po->edid_len;
		if (info) {
			if (drm_edid_parse(o->edid, (unsigned)o->edid_len, info) == 0) {
				o->hdmi_sink = info->is_hdmi;
				o->has_audio = info->has_audio;
			}
			kfree(info);
		}
	}
	if (SDVO_IS_LVDS(po->flag)) {
		o->is_panel = 1;
		o->fixed_mode = po->fixed;
		o->fixed_mode_valid = 1;
		o->mm_width = po->mm_w;
		o->mm_height = po->mm_h;
		o->detected = 1;
	}
	o->priv = po;
	if (po->edid) {
		kfree(po->edid);
		po->edid = NULL;
		po->edid_len = 0;
	}

	if (SDVO_IS_LVDS(po->flag))
		kprintf("[drm] i915: %s: panel %ux%u, %u kHz\n", o->name, o->fixed_mode.hdisplay,
			o->fixed_mode.vdisplay, o->fixed_mode.clock);
	else if (SDVO_IS_TV(po->flag))
		kprintf("[drm] i915: %s: TV output (%s)\n", o->name,
			sdvo_tv_format_names[po->tv_format]);
	else
		kprintf("[drm] i915: %s: %s output%s\n", o->name,
			SDVO_IS_TMDS(po->flag) ? (po->is_hdmi ? "HDMI" : "DVI") : "VGA",
			po->hotplug ? ", hotplug" : "");
}

int lg_sdvo_init(struct lg_display *d, uint32_t reg, int port)
{
	static const uint16_t probe_order[] = {
		SDVO_OUTPUT_TMDS0, SDVO_OUTPUT_TMDS1,
		/* TV has no XXX1 function block */
		SDVO_OUTPUT_SVID0, SDVO_OUTPUT_CVBS0, SDVO_OUTPUT_YPRPB0, SDVO_OUTPUT_RGB0,
		SDVO_OUTPUT_RGB1, SDVO_OUTPUT_LVDS0, SDVO_OUTPUT_LVDS1,
	};
	struct sdvo_output *stage[sizeof(probe_order) / sizeof(probe_order[0])];
	struct sdvo_dev *s;
	uint16_t flags;
	int nstage = 0, made = 0, i;
	unsigned k;

	if (d->pch != LG_PCH_NONE ? port != LG_PORT_B :
				    (port != LG_PORT_B && port != LG_PORT_C)) {
		kprintf("[drm] i915: SDVO %c is not a port of this platform\n", 'A' + port);
		return 0;
	}

	s = kalloc(sizeof(*s));
	if (!s)
		return 0;
	mm_memset(s, 0, sizeof(*s));
	s->d = d;
	s->port = port;
	s->reg = reg;
	s->first_out = -1;
	s->addr = (uint8_t)(sdvo_get_target_addr(d, port) >> 1);

	sdvo_select_i2c_bus(s);
	if (!s->i2c)
		goto err;

	/* Read the registers to see whether anything answers. */
	for (i = 0; i < 0x40; i++) {
		uint8_t byte;

		if (!sdvo_read_byte(s, (uint8_t)i, &byte)) {
			i915_dbg("[drm] i915: No SDVO device found on SDVO%c\n", sdvo_port_name(s));
			goto err;
		}
	}

	if (!sdvo_get_capabilities(s))
		goto err;

	s->colorimetry_cap = sdvo_get_colorimetry_cap(s);

	for (i = 0; i < 3; i++)
		sdvo_init_ddc_proxy(&s->ddc[i], s, i + 1);

	flags = sdvo_filter_output_flags(s->caps.output_flags);
	if (flags == 0) {
		i915_dbg("[drm] i915: SDVO%c: unknown SDVO output type (0x%04x)\n",
			 sdvo_port_name(s), s->caps.output_flags);
		goto err;
	}

	for (k = 0; k < sizeof(probe_order) / sizeof(probe_order[0]); k++) {
		uint16_t type = flags & probe_order[k];

		if (!type)
			continue;
		stage[nstage] = sdvo_output_prepare(s, type);
		if (!stage[nstage]) {
			i915_dbg("[drm] i915: SDVO output failed to setup on SDVO%c\n",
				 sdvo_port_name(s));
			goto err_output;
		}
		nstage++;
	}

	/* the input timing to the screen, on input 0 */
	if (!sdvo_set_target_input(s))
		goto err_output;
	if (!sdvo_get_input_pixel_clock_range(s, &s->pixel_clock_min, &s->pixel_clock_max))
		goto err_output;

	i915_dbg("[drm] i915: SDVO%c device VID/DID: %02X:%02X.%02X, clock range %dMHz - %dMHz, "
		 "num inputs: %u, output 1: %c, output 2: %c\n",
		 sdvo_port_name(s), s->caps.vendor_id, s->caps.device_id, s->caps.device_rev_id,
		 s->pixel_clock_min / 1000, s->pixel_clock_max / 1000,
		 SDVO_CAPS_NUM_INPUTS(&s->caps),
		 (s->caps.output_flags & (SDVO_OUTPUT_TMDS0 | SDVO_OUTPUT_RGB0 | SDVO_OUTPUT_LVDS0 |
					  SDVO_OUTPUT_SVID0 | SDVO_OUTPUT_CVBS0 |
					  SDVO_OUTPUT_YPRPB0)) ? 'Y' : 'N',
		 (s->caps.output_flags & (SDVO_OUTPUT_TMDS1 | SDVO_OUTPUT_RGB1 |
					  SDVO_OUTPUT_LVDS1)) ? 'Y' : 'N');

	kprintf("[drm] i915: SDVO-%c: encoder %02x:%02x.%02x, %d-%d MHz\n", sdvo_port_name(s),
		s->caps.vendor_id, s->caps.device_id, s->caps.device_rev_id,
		s->pixel_clock_min / 1000, s->pixel_clock_max / 1000);

	g_sdvo[s->port == LG_PORT_B ? 0 : 1] = s;

	for (i = 0; i < nstage; i++) {
		struct lg_output *o = lg_output_new(d);

		if (!o) {
			kprintf("[drm] i915: SDVO-%c: no output slot left\n", sdvo_port_name(s));
			for (; i < nstage; i++)
				sdvo_output_free(stage[i]);
			break;
		}
		if (s->first_out < 0)
			s->first_out = (int)(o - d->out);
		sdvo_output_create(d, s, stage[i], o);
		made++;
	}

	if (!made) {
		g_sdvo[s->port == LG_PORT_B ? 0 : 1] = NULL;
		goto err;
	}
	return 1;

err_output:
	for (i = 0; i < nstage; i++)
		sdvo_output_free(stage[i]);
err:
	sdvo_unselect_i2c_bus(s);
	kfree(s);
	return 0;
}
