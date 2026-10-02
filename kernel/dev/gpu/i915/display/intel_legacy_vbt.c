// LikeOS -- the Video BIOS Tables of the Intel parts before DDI.
//
// The board's display wiring is not discoverable from the hardware: which
// ports are routed to connectors, which SDVO encoder chip sits on which
// port at which I2C address, whether there is an LVDS, eDP or DSI panel,
// its native timing, power sequencing delays, link parameters and
// backlight PWM.  The video BIOS describes all of it in the VBT, a "$VBT"
// header followed by a BIOS data block header and a chain of tagged
// blocks.  The table sits in the ACPI OpRegion on most machines; on older
// ones only the video BIOS image itself carries it, which the platform
// firmware shadows at 0xC0000 for the boot display device.
//
// The layout of several blocks changed with the BDB version (the parts
// here carry versions from about 100 to 200): the child device entries
// grew, the LFP data tables gained pointer blocks, the eDP block gained
// per-panel fields.  Fields read past the end of a block read as zero,
// and the parser follows the version where a field's meaning moved.
// Without any VBT the defaults let every output path run: SSC on at the
// platform's usual frequency, LVDS, TV and VGA assumed possible, and on
// Cherryview a DP/HDMI child device made up for each port.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2006-2024 Intel Corporation

#include <kernel/dev/gpu/i915/intel_legacy.h>
#include <kernel/dev/gpu/drm_edid.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

/* ---- table layout -------------------------------------------------------------- */

/* The VBT header ("$VBT") and the BDB header that follows it. */
#define VBT_HDR_SIZE 48
#define VBT_HDR_VBT_SIZE 24 /* u16: header, BDB header and blocks */
#define VBT_HDR_BDB_OFFSET 28 /* u32: from the start of the VBT */
#define VBT_BDB_HDR_SIZE 22
#define VBT_BDB_VERSION 16 /* u16 */
#define VBT_BDB_HEADER_SIZE 18 /* u16 */
#define VBT_BDB_BDB_SIZE 20 /* u16: BDB header and blocks */

/* Block ids */
#define VBT_BDB_GENERAL_FEATURES 1
#define VBT_BDB_GENERAL_DEFINITIONS 2
#define VBT_BDB_DRIVER_FEATURES 12
#define VBT_BDB_SDVO_LVDS_OPTIONS 22
#define VBT_BDB_SDVO_LVDS_DTD 23
#define VBT_BDB_EDP 27
#define VBT_BDB_LFP_OPTIONS 40
#define VBT_BDB_LFP_DATA_PTRS 41
#define VBT_BDB_LFP_DATA 42
#define VBT_BDB_LFP_BACKLIGHT 43
#define VBT_BDB_MIPI_SEQUENCE 53

/* Child device types: the old exact codes and the bits of the 915+ ones */
#define VBT_DEVICE_TYPE_TV 0x0009
#define VBT_DEVICE_TYPE_LFP 0x0022
#define VBT_DEVICE_TYPE_TV_SVIDEO_COMPOSITE 0x0609
#define VBT_DEVICE_TYPE_INT_LFP 0x1022
#define VBT_DEVICE_TYPE_INT_TV 0x1009
#define VBT_DEVICE_TYPE_INTERNAL_CONNECTOR (1u << 12)
#define VBT_DEVICE_TYPE_NOT_HDMI_OUTPUT (1u << 11)
#define VBT_DEVICE_TYPE_MIPI_OUTPUT (1u << 10)
#define VBT_DEVICE_TYPE_TMDS_DVI_SIGNALING (1u << 4)
#define VBT_DEVICE_TYPE_DISPLAYPORT_OUTPUT (1u << 2)

/* dvo_port before BDB 155 (SDVO encoders) ... */
#define VBT_DEVICE_PORT_DVOB 0x01
#define VBT_DEVICE_PORT_DVOC 0x02
/* ... and from 155 on */
#define VBT_DVO_PORT_HDMIA 0
#define VBT_DVO_PORT_HDMIB 1
#define VBT_DVO_PORT_HDMIC 2
#define VBT_DVO_PORT_HDMID 3
#define VBT_DVO_PORT_DPB 7
#define VBT_DVO_PORT_DPC 8
#define VBT_DVO_PORT_DPD 9
#define VBT_DVO_PORT_DPA 10
#define VBT_DVO_PORT_MIPIA 21
#define VBT_DVO_PORT_MIPIC 23

/* SDVO encoders answer at these (8-bit) addresses. */
#define VBT_SDVO_ADDR1 0x70
#define VBT_SDVO_ADDR2 0x72

/* AUX channel codes */
#define VBT_DP_AUX_A 0x40
#define VBT_DP_AUX_B 0x10
#define VBT_DP_AUX_C 0x20
#define VBT_DP_AUX_D 0x30

/* Child device fields (byte offsets) */
#define VBT_CHILD_HANDLE 0
#define VBT_CHILD_DEVICE_TYPE 2
#define VBT_CHILD_HDMI_LS 7 /* hdmi_level_shifter_value:5, 158+ */
#define VBT_CHILD_ADDIN_OFFSET 14
#define VBT_CHILD_DVO_PORT 16
#define VBT_CHILD_I2C_PIN 17
#define VBT_CHILD_TARGET_ADDR 18
#define VBT_CHILD_DDC_PIN 19
#define VBT_CHILD_DVO_CFG 22
#define VBT_CHILD_FLAGS0 23 /* bit 1: lane_reversal, 184+ */
#define VBT_CHILD_AUX_CHANNEL 25
#define VBT_CHILD_DVO_WIRING 28
#define VBT_LEGACY_CHILD_SIZE 33

/* Block 12: lvds_config, bits 11-12 of the u16 at byte 7 */
#define VBT_DRIVER_FEATURE_INT_LVDS 1
#define VBT_DRIVER_FEATURE_INT_SDVO_LVDS 3

/* Block 43: the per-panel controller type the PWM backlight defaults to */
#define VBT_BACKLIGHT_TYPE_PWM 2
#define VBT_BL_CONTROL_DISPLAY 2

/* The sizes of the LFP data tables */
#define VBT_DTD_SIZE 18
#define VBT_PNP_ID_SIZE 10
#define VBT_PANEL_NAME_SIZE 13

/* ---- state that does not fit struct lg_vbt ----------------------------------------- */

/* The child devices' add-in data offsets (a non-zero one marks a device
 * the video BIOS really set up). */
static uint16_t g_addin[LG_VBT_MAX_CHILDREN];
/* The VBT came from the OpRegion, i.e. the platform firmware vouches for
 * it. */
static int g_vbt_from_opregion;

/* ---- reading -------------------------------------------------------------------- */

/* Bounded little-endian reads: past `len' everything reads as zero, which
 * is what an older, shorter block means. */
static uint8_t rd8(const uint8_t *p, uint32_t len, uint32_t off)
{
	return off < len ? p[off] : 0;
}

static uint16_t rd16(const uint8_t *p, uint32_t len, uint32_t off)
{
	return (uint16_t)(rd8(p, len, off) | (rd8(p, len, off + 1) << 8));
}

static uint32_t rd32(const uint8_t *p, uint32_t len, uint32_t off)
{
	return (uint32_t)rd16(p, len, off) | ((uint32_t)rd16(p, len, off + 2) << 16);
}

static uint64_t rd64(const uint8_t *p, uint32_t len, uint32_t off)
{
	return (uint64_t)rd32(p, len, off) | ((uint64_t)rd32(p, len, off + 4) << 32);
}

/* 2 bits per panel, panel 0 lowest. */
static uint32_t panel_bits(uint32_t value, int panel_type, int num_bits)
{
	return (value >> (panel_type * num_bits)) & ((1u << num_bits) - 1);
}

/* ---- finding the table ---------------------------------------------------------- */

/* Is there a whole VBT at `buf' within `size' bytes? */
static int vbt_is_valid(const uint8_t *buf, uint32_t size)
{
	uint32_t vbt_size, bdb_offset, bdb_size;

	if (!buf || size < VBT_HDR_SIZE) {
		i915_dbg("[drm] i915: VBT header incomplete\n");
		return 0;
	}
	if (buf[0] != '$' || buf[1] != 'V' || buf[2] != 'B' || buf[3] != 'T') {
		i915_dbg("[drm] i915: VBT invalid signature\n");
		return 0;
	}
	vbt_size = rd16(buf, size, VBT_HDR_VBT_SIZE);
	if (vbt_size > size) {
		i915_dbg("[drm] i915: VBT incomplete (vbt_size overflows)\n");
		return 0;
	}
	size = vbt_size;
	bdb_offset = rd32(buf, size, VBT_HDR_BDB_OFFSET);
	if (bdb_offset > size || size - bdb_offset < VBT_BDB_HDR_SIZE) {
		i915_dbg("[drm] i915: BDB header incomplete\n");
		return 0;
	}
	bdb_size = rd16(buf, size, bdb_offset + VBT_BDB_BDB_SIZE);
	if (bdb_size > size - bdb_offset) {
		i915_dbg("[drm] i915: BDB incomplete\n");
		return 0;
	}
	return 1;
}

/* The legacy option ROM shadow of the boot display device. */
#define VBT_ROM_SHADOW_PHYS 0xc0000u
#define VBT_ROM_SHADOW_MAX 0x20000u

/* The size the ROM declares: its images (each a 0x55AA header with a
 * "PCIR" data structure giving the length in 512-byte units and a
 * last-image flag) back to back, or the length byte of a ROM without a
 * data structure. */
static uint32_t rom_size(const uint8_t *rom)
{
	uint32_t off = 0;

	if (rom[0] != 0x55 || rom[1] != 0xaa)
		return 0;
	for (;;) {
		uint32_t pds, length;
		int last;

		if (off + 0x1a > VBT_ROM_SHADOW_MAX || rom[off] != 0x55 || rom[off + 1] != 0xaa)
			break;
		pds = off + (rom[off + 0x18] | ((uint32_t)rom[off + 0x19] << 8));
		if (pds + 0x18 > VBT_ROM_SHADOW_MAX || rom[pds] != 'P' || rom[pds + 1] != 'C' ||
		    rom[pds + 2] != 'I' || rom[pds + 3] != 'R')
			break;
		last = rom[pds + 0x15] & 0x80;
		length = (rom[pds + 0x10] | ((uint32_t)rom[pds + 0x11] << 8)) * 512u;
		off += length;
		if (!length || last || off >= VBT_ROM_SHADOW_MAX)
			break;
	}
	if (!off)
		off = (uint32_t)rom[2] * 512u;
	return off < VBT_ROM_SHADOW_MAX ? off : VBT_ROM_SHADOW_MAX;
}

/* The VBT in the video BIOS image: the first "$VBT" within the ROM. */
static const uint8_t *rom_get_vbt(uint32_t *sizep)
{
	const uint8_t *rom = phys_to_virt(VBT_ROM_SHADOW_PHYS);
	uint32_t size = rom_size(rom);
	uint32_t off, vbt_size;

	if (!size)
		return NULL;
	for (off = 0; off + 4 <= size; off++)
		if (rom[off] == '$' && rom[off + 1] == 'V' && rom[off + 2] == 'B' &&
		    rom[off + 3] == 'T')
			break;
	if (off + 4 > size)
		return NULL;
	if (size - off < VBT_HDR_SIZE) {
		i915_dbg("[drm] i915: VBT header incomplete\n");
		return NULL;
	}
	vbt_size = rd16(rom + off, size - off, VBT_HDR_VBT_SIZE);
	if (vbt_size > size - off) {
		i915_dbg("[drm] i915: VBT incomplete (vbt_size overflows)\n");
		return NULL;
	}
	if (!vbt_is_valid(rom + off, vbt_size))
		return NULL;
	*sizep = vbt_size;
	return rom + off;
}

/* ---- the block chain ------------------------------------------------------------ */

static const uint8_t *bdb_of(struct lg_display *d, uint32_t *total, uint32_t *hdr)
{
	const uint8_t *raw = d->vbt.raw;
	uint32_t off;

	if (!raw)
		return NULL;
	off = rd32(raw, d->vbt.raw_len, VBT_HDR_BDB_OFFSET);
	*total = rd16(raw, d->vbt.raw_len, off + VBT_BDB_BDB_SIZE);
	*hdr = rd16(raw, d->vbt.raw_len, off + VBT_BDB_HEADER_SIZE);
	return raw + off;
}

/* A block's size from its 3-byte header; the MIPI sequence block from
 * version 3 on keeps a 32-bit size after its version byte. */
static uint32_t block_size_at(const uint8_t *bdb, uint32_t total, uint32_t index)
{
	if (rd8(bdb, total, index) == VBT_BDB_MIPI_SEQUENCE && rd8(bdb, total, index + 3) >= 3)
		return rd32(bdb, total, index + 4);
	return rd16(bdb, total, index + 1);
}

/* The offset of block `id''s body from the start of the BDB, 0 when it is
 * not there; *sizep its size. */
static uint32_t find_section(struct lg_display *d, int id, uint32_t *sizep)
{
	uint32_t total, index, size;
	const uint8_t *bdb = bdb_of(d, &total, &index);

	if (!bdb)
		return 0;
	while (index + 3 < total) {
		int cur = bdb[index];

		size = block_size_at(bdb, total, index);
		index += 3;
		if (size > total - index)
			return 0;
		if (cur == id) {
			*sizep = size;
			return index;
		}
		index += size;
	}
	return 0;
}

const uint8_t *lg_vbt_block(struct lg_display *d, int id, uint32_t *len)
{
	uint32_t total, hdr, size = 0, off;
	const uint8_t *bdb = bdb_of(d, &total, &hdr);

	off = find_section(d, id, &size);
	if (!bdb || !off)
		return NULL;
	/* The MIPI sequence block's version and 32-bit size count as part
	 * of its body. */
	if (id == VBT_BDB_MIPI_SEQUENCE && bdb[off] >= 3) {
		size += 5;
		if (size > total - off)
			size = total - off;
	}
	if (len)
		*len = size;
	return bdb + off;
}

/* ---- block 1: general features ------------------------------------------------------ */

/* The spread-spectrum clock the LVDS PLL runs from, in kHz: the
 * "alternate" bit picks the second frequency the platform has. */
static uint32_t ssc_frequency(struct lg_display *d, int alternate)
{
	switch (d->ver) {
	case 2:
		return alternate ? 66667 : 48000;
	case 3:
	case 4:
		return alternate ? 100000 : 96000;
	default:
		return alternate ? 100000 : 120000;
	}
}

static void parse_general_features(struct lg_display *d)
{
	uint32_t len = 0;
	const uint8_t *b = lg_vbt_block(d, VBT_BDB_GENERAL_FEATURES, &len);
	uint8_t bits2, bits3, bits5;

	if (!b)
		return;
	bits2 = rd8(b, len, 1);
	bits3 = rd8(b, len, 2);
	bits5 = rd8(b, len, 4);

	d->vbt.int_tv_support = (bits5 >> 1) & 1;
	/* int_crt_support cannot be trusted on the older parts */
	if (d->vbt.version >= 155 && d->is_vlv)
		d->vbt.int_crt_support = bits5 & 1;
	d->vbt.lvds_use_ssc = (bits2 >> 1) & 1;
	d->vbt.lvds_ssc_freq_khz = ssc_frequency(d, (bits2 >> 2) & 1);
	d->vbt.display_clock_mode = (bits2 >> 6) & 1;
	d->vbt.fdi_rx_polarity_inverted = (bits3 >> 3) & 1;

	i915_dbg("[drm] i915: VBT general features: int_tv %d int_crt %d lvds_ssc %d at %u kHz, "
		 "display_clock_mode %d fdi_rx_polarity_inverted %d\n",
		 d->vbt.int_tv_support, d->vbt.int_crt_support, d->vbt.lvds_use_ssc,
		 d->vbt.lvds_ssc_freq_khz, d->vbt.display_clock_mode,
		 d->vbt.fdi_rx_polarity_inverted);
}

/* ---- block 2: general definitions and the child devices --------------------------------- */

/* The child device size each BDB version uses. */
static int child_expected_size(uint16_t version)
{
	if (version > 264)
		return -1;
	if (version >= 263)
		return 44;
	if (version >= 256)
		return 40;
	if (version >= 216)
		return 39;
	if (version >= 196)
		return 38;
	if (version >= 195)
		return 37;
	if (version >= 111)
		return VBT_LEGACY_CHILD_SIZE;
	if (version >= 106)
		return 27;
	return 22;
}

static uint8_t map_aux_ch(uint8_t aux)
{
	switch (aux) {
	case VBT_DP_AUX_A:
		return LG_PORT_A;
	case VBT_DP_AUX_B:
		return LG_PORT_B;
	case VBT_DP_AUX_C:
		return LG_PORT_C;
	case VBT_DP_AUX_D:
		return LG_PORT_D;
	default:
		if (aux)
			i915_dbg("[drm] i915: ignoring VBT AUX CH 0x%x\n", aux);
		return 0xff;
	}
}

/* The integrated port a child device's dvo_port names (each port answers
 * to its HDMI and its DP code), -1 for none. */
static int dvo_port_to_port(uint8_t dvo_port)
{
	switch (dvo_port) {
	case VBT_DVO_PORT_HDMIA:
	case VBT_DVO_PORT_DPA:
		return LG_PORT_A;
	case VBT_DVO_PORT_HDMIB:
	case VBT_DVO_PORT_DPB:
		return LG_PORT_B;
	case VBT_DVO_PORT_HDMIC:
	case VBT_DVO_PORT_DPC:
		return LG_PORT_C;
	case VBT_DVO_PORT_HDMID:
	case VBT_DVO_PORT_DPD:
		return LG_PORT_D;
	default:
		return -1;
	}
}

static void child_update_caps(struct lg_vbt_child *c)
{
	c->supports_dvi = !!(c->device_type & VBT_DEVICE_TYPE_TMDS_DVI_SIGNALING);
	c->supports_hdmi = c->supports_dvi && !(c->device_type & VBT_DEVICE_TYPE_NOT_HDMI_OUTPUT);
	c->supports_dp = !!(c->device_type & VBT_DEVICE_TYPE_DISPLAYPORT_OUTPUT);
	c->is_internal = !!(c->device_type & VBT_DEVICE_TYPE_INTERNAL_CONNECTOR);
	c->supports_edp = c->supports_dp && c->is_internal;
}

static void parse_child(struct lg_display *d, const uint8_t *p, uint32_t size,
			struct lg_vbt_child *c, uint16_t *addin)
{
	uint16_t version = d->vbt.version;

	mm_memset(c, 0, sizeof(*c));
	c->handle = rd16(p, size, VBT_CHILD_HANDLE);
	c->device_type = rd16(p, size, VBT_CHILD_DEVICE_TYPE);
	c->dvo_port = rd8(p, size, VBT_CHILD_DVO_PORT);
	c->i2c_pin = rd8(p, size, VBT_CHILD_I2C_PIN);
	c->slave_addr = rd8(p, size, VBT_CHILD_TARGET_ADDR);
	c->ddc_pin = rd8(p, size, VBT_CHILD_DDC_PIN);
	c->dvo_cfg = rd8(p, size, VBT_CHILD_DVO_CFG);
	c->dvo_wiring = rd8(p, size, VBT_CHILD_DVO_WIRING);
	c->aux_ch = map_aux_ch(rd8(p, size, VBT_CHILD_AUX_CHANNEL));
	c->hdmi_level_shift = version >= 158 ? (rd8(p, size, VBT_CHILD_HDMI_LS) & 0x1f) : 0xff;
	c->lane_reversal = version >= 184 ? ((rd8(p, size, VBT_CHILD_FLAGS0) >> 1) & 1) : 0;
	c->port = dvo_port_to_port(c->dvo_port);
	child_update_caps(c);
	*addin = rd16(p, size, VBT_CHILD_ADDIN_OFFSET);
}

static void parse_general_definitions(struct lg_display *d)
{
	uint32_t len = 0;
	const uint8_t *b = lg_vbt_block(d, VBT_BDB_GENERAL_DEFINITIONS, &len);
	uint32_t child_size, n, i;
	uint8_t bus_pin;
	int expected;

	if (!b) {
		i915_dbg("[drm] i915: no general definition block, no devices defined\n");
		return;
	}
	if (len < 5) {
		i915_dbg("[drm] i915: general definitions block too small (%u)\n", len);
		return;
	}

	bus_pin = b[0];
	i915_dbg("[drm] i915: VBT crt_ddc_bus_pin %u\n", bus_pin);
	if (lg_gmbus_pin_valid(d, bus_pin))
		d->vbt.crt_ddc_pin = bus_pin;

	child_size = b[4];
	expected = child_expected_size(d->vbt.version);
	if (expected < 0)
		expected = 44;
	if ((int)child_size != expected)
		kprintf("[drm] i915: unexpected VBT child device size %u (expected %d for "
			"version %u)\n",
			child_size, expected, d->vbt.version);
	/* the legacy child device is the least the parser needs */
	if (child_size < VBT_LEGACY_CHILD_SIZE) {
		i915_dbg("[drm] i915: VBT child device size %u too small\n", child_size);
		return;
	}

	n = (len - 5) / child_size;
	for (i = 0; i < n; i++) {
		const uint8_t *p = b + 5 + i * child_size;
		struct lg_vbt_child *c;

		if (!rd16(p, child_size, VBT_CHILD_DEVICE_TYPE))
			continue;
		if (d->vbt.nchild >= LG_VBT_MAX_CHILDREN) {
			kprintf("[drm] i915: VBT has more than %d child devices\n",
				LG_VBT_MAX_CHILDREN);
			break;
		}
		c = &d->vbt.child[d->vbt.nchild];
		parse_child(d, p, child_size, c, &g_addin[d->vbt.nchild]);
		d->vbt.nchild++;
		i915_dbg("[drm] i915: VBT child device: type 0x%04x dvo_port %u i2c %u addr 0x%02x "
			 "ddc %u aux %u\n",
			 c->device_type, c->dvo_port, c->i2c_pin, c->slave_addr, c->ddc_pin,
			 c->aux_ch);
	}
	if (!d->vbt.nchild)
		i915_dbg("[drm] i915: no child device parsed from VBT\n");
}

/* SDVO encoders: the child devices on SDVO B or C at one of the SDVO
 * addresses (only the generations that can have SDVO). */
static void parse_sdvo_device_mapping(struct lg_display *d)
{
	int i, count = 0;

	if (d->ver < 3 || d->ver > 7)
		return;
	for (i = 0; i < d->vbt.nchild; i++) {
		const struct lg_vbt_child *c = &d->vbt.child[i];
		int idx;

		if (c->slave_addr != VBT_SDVO_ADDR1 && c->slave_addr != VBT_SDVO_ADDR2)
			continue;
		if (c->dvo_port != VBT_DEVICE_PORT_DVOB && c->dvo_port != VBT_DEVICE_PORT_DVOC) {
			i915_dbg("[drm] i915: VBT: incorrect SDVO port, skipped\n");
			continue;
		}
		idx = c->dvo_port - 1;
		if (!d->vbt.sdvo[idx].initialized) {
			d->vbt.sdvo[idx].dvo_port = c->dvo_port;
			d->vbt.sdvo[idx].slave_addr = c->slave_addr;
			d->vbt.sdvo[idx].dvo_wiring = c->dvo_wiring;
			d->vbt.sdvo[idx].ddc_pin = c->ddc_pin;
			d->vbt.sdvo[idx].i2c_pin = c->i2c_pin;
			d->vbt.sdvo[idx].initialized = 1;
			i915_dbg("[drm] i915: SDVO device on SDVO%c: addr 0x%02x wiring %u ddc %u "
				 "i2c %u\n",
				 idx ? 'C' : 'B', c->slave_addr, c->dvo_wiring, c->ddc_pin,
				 c->i2c_pin);
		} else {
			i915_dbg("[drm] i915: one SDVO port shared by two SDVO devices?\n");
		}
		count++;
	}
	if (!count)
		i915_dbg("[drm] i915: no SDVO device info in VBT\n");
}

/* G4X on: the port A child cannot carry TMDS (port A is the CPU eDP
 * port), whatever the table claims. */
static int has_ddi_port_info(struct lg_display *d)
{
	return d->ver >= 5 || d->is_g4x || d->is_gm45;
}

static void parse_ddi_ports(struct lg_display *d)
{
	int i;

	if (!has_ddi_port_info(d))
		return;
	for (i = 0; i < d->vbt.nchild; i++) {
		struct lg_vbt_child *c = &d->vbt.child[i];

		if (c->port != LG_PORT_A || !c->supports_dvi)
			continue;
		i915_dbg("[drm] i915: VBT claims port A supports DVI%s, ignoring\n",
			 c->supports_hdmi ? "/HDMI" : "");
		c->device_type &= (uint16_t)~VBT_DEVICE_TYPE_TMDS_DVI_SIGNALING;
		c->device_type |= VBT_DEVICE_TYPE_NOT_HDMI_OUTPUT;
		child_update_caps(c);
	}
}

/* ---- block 12: driver features ------------------------------------------------------ */

static void parse_driver_features(struct lg_display *d)
{
	uint32_t len = 0;
	const uint8_t *b = lg_vbt_block(d, VBT_BDB_DRIVER_FEATURES, &len);
	uint32_t lvds_config;

	if (!b)
		return;
	lvds_config = (rd16(b, len, 7) >> 11) & 3;

	if (d->ver >= 5) {
		/* The "integrated SDVO LVDS" value stands for eDP on these
		 * parts, whatever the specification says: only a plain LVDS
		 * value means LVDS. */
		if (lvds_config != VBT_DRIVER_FEATURE_INT_LVDS)
			d->vbt.int_lvds_support = 0;
	} else {
		/* The bits are only populated from about version 134 on. */
		if (d->vbt.version >= 134 && lvds_config != VBT_DRIVER_FEATURE_INT_LVDS &&
		    lvds_config != VBT_DRIVER_FEATURE_INT_SDVO_LVDS)
			d->vbt.int_lvds_support = 0;
	}
}

/* ---- the panel ---------------------------------------------------------------- */

/* An 18-byte detailed timing descriptor into a mode. */
static int fill_detail_timing(const uint8_t *t, struct drm_mode_modeinfo *m,
			      uint32_t *width_mm, uint32_t *height_mm)
{
	uint32_t hactive, hblank, hsync_off, hsync_pw;
	uint32_t vactive, vblank, vsync_off, vsync_pw;

	mm_memset(m, 0, sizeof(*m));
	hactive = t[2] | ((uint32_t)(t[4] >> 4) << 8);
	hblank = t[3] | ((uint32_t)(t[4] & 0xf) << 8);
	vactive = t[5] | ((uint32_t)(t[7] >> 4) << 8);
	vblank = t[6] | ((uint32_t)(t[7] & 0xf) << 8);
	hsync_off = t[8] | ((uint32_t)((t[11] >> 6) & 3) << 8);
	hsync_pw = t[9] | ((uint32_t)((t[11] >> 4) & 3) << 8);
	vsync_off = (t[10] >> 4) | ((uint32_t)((t[11] >> 2) & 3) << 4);
	vsync_pw = (t[10] & 0xf) | ((uint32_t)(t[11] & 3) << 4);

	m->hdisplay = (uint16_t)hactive;
	m->hsync_start = (uint16_t)(hactive + hsync_off);
	m->hsync_end = (uint16_t)(m->hsync_start + hsync_pw);
	m->htotal = (uint16_t)(hactive + hblank);
	m->vdisplay = (uint16_t)vactive;
	m->vsync_start = (uint16_t)(vactive + vsync_off);
	m->vsync_end = (uint16_t)(m->vsync_start + vsync_pw);
	m->vtotal = (uint16_t)(vactive + vblank);
	m->clock = (uint32_t)(t[0] | (t[1] << 8)) * 10;
	m->type = DRM_MODE_TYPE_PREFERRED;
	m->flags |= (t[17] & (1u << 6)) ? DRM_MODE_FLAG_PHSYNC : DRM_MODE_FLAG_NHSYNC;
	m->flags |= (t[17] & (1u << 5)) ? DRM_MODE_FLAG_PVSYNC : DRM_MODE_FLAG_NVSYNC;
	if (width_mm)
		*width_mm = t[12] | ((uint32_t)(t[14] >> 4) << 8);
	if (height_mm)
		*height_mm = t[13] | ((uint32_t)(t[14] & 0xf) << 8);

	/* some tables have bogus sync ends */
	if (m->hsync_end > m->htotal) {
		i915_dbg("[drm] i915: VBT: reducing hsync_end %u->%u\n", m->hsync_end, m->htotal);
		m->hsync_end = m->htotal;
	}
	if (m->vsync_end > m->vtotal) {
		i915_dbg("[drm] i915: VBT: reducing vsync_end %u->%u\n", m->vsync_end, m->vtotal);
		m->vsync_end = m->vtotal;
	}
	if (!m->hdisplay || !m->vdisplay || !m->clock || !m->htotal || !m->vtotal)
		return 0;
	drm_mode_finish(m);
	return 1;
}

/* Which of the 16 panel entries this machine has: the table's own
 * choice when it is a valid index, else the first entry.  (A table that
 * defers to the panel's EDID ID, 0xff, gets the first entry too: no EDID
 * has been read when the table is parsed.) */
static int get_panel_type(struct lg_display *d)
{
	uint32_t len = 0;
	const uint8_t *b = lg_vbt_block(d, VBT_BDB_LFP_OPTIONS, &len);
	int pt;

	if (!b)
		return 0;
	pt = rd8(b, len, 0);
	if (pt < 16)
		return pt;
	if (pt == 0xff)
		i915_dbg("[drm] i915: VBT panel type by PNP ID, using the fallback\n");
	else
		i915_dbg("[drm] i915: invalid VBT panel type 0x%x\n", pt);
	return 0;
}

static void parse_panel_options(struct lg_display *d)
{
	uint32_t len = 0;
	const uint8_t *b = lg_vbt_block(d, VBT_BDB_LFP_OPTIONS, &len);

	if (!b)
		return;
	d->vbt.lvds_dither = (rd8(b, len, 2) >> 5) & 1;
}

/* The LFP data table pointers, made relative to the LFP data block. */
struct lfp_tbl {
	int off, size;
};

struct lfp_ptrs {
	int num_entries;
	struct {
		struct lfp_tbl fp, dvo, pnp;
	} p[16];
	struct lfp_tbl name;
};

static void lfp_tbl_read(const uint8_t *b, uint32_t len, uint32_t off, struct lfp_tbl *t)
{
	t->off = rd16(b, len, off);
	t->size = rd8(b, len, off + 2);
}

static int validate_lfp_data_ptrs(const struct lfp_ptrs *ptrs, const uint8_t *data,
				  uint32_t data_size)
{
	int fp_size, dvo_size, pnp_size, name_size, lfp_data_size, i;

	if (!data_size)
		return 0;
	/* always three tables: fp_timing, dvo_timing, panel PNP ID */
	if (ptrs->num_entries != 3)
		return 0;

	fp_size = ptrs->p[0].fp.size;
	dvo_size = ptrs->p[0].dvo.size;
	pnp_size = ptrs->p[0].pnp.size;
	name_size = ptrs->name.size;

	/* fp_timing has a variable size */
	if (fp_size < 32 || dvo_size != VBT_DTD_SIZE || pnp_size != VBT_PNP_ID_SIZE)
		return 0;
	/* the panel names are not in old tables */
	if (name_size != 0 && name_size != VBT_PANEL_NAME_SIZE)
		return 0;

	lfp_data_size = ptrs->p[1].fp.off - ptrs->p[0].fp.off;
	if (lfp_data_size <= 0 || 16 * lfp_data_size > (int)data_size)
		return 0;

	/* the entries must all have the same shape */
	for (i = 1; i < 16; i++) {
		if (ptrs->p[i].fp.size != fp_size || ptrs->p[i].dvo.size != dvo_size ||
		    ptrs->p[i].pnp.size != pnp_size)
			return 0;
		if (ptrs->p[i].fp.off - ptrs->p[i - 1].fp.off != lfp_data_size ||
		    ptrs->p[i].dvo.off - ptrs->p[i - 1].dvo.off != lfp_data_size ||
		    ptrs->p[i].pnp.off - ptrs->p[i - 1].pnp.off != lfp_data_size)
			return 0;
	}

	/* Except on Valleyview/Cherryview, real tables have 6 bytes more in
	 * fp_timing than it declares (its 0xffff terminator falls in them). */
	if (fp_size + 6 + dvo_size + pnp_size == lfp_data_size)
		fp_size += 6;
	if (fp_size + dvo_size + pnp_size != lfp_data_size)
		return 0;
	if (ptrs->p[0].fp.off + fp_size != ptrs->p[0].dvo.off ||
	    ptrs->p[0].dvo.off + dvo_size != ptrs->p[0].pnp.off ||
	    ptrs->p[0].pnp.off + pnp_size != lfp_data_size)
		return 0;

	/* the tables must lie inside the data block */
	for (i = 0; i < 16; i++) {
		if (ptrs->p[i].fp.off + fp_size > (int)data_size ||
		    ptrs->p[i].dvo.off + dvo_size > (int)data_size ||
		    ptrs->p[i].pnp.off + pnp_size > (int)data_size)
			return 0;
	}
	if (ptrs->name.off + 16 * name_size > (int)data_size)
		return 0;

	/* and every fp_timing must end in its terminator */
	for (i = 0; i < 16; i++) {
		int t = ptrs->p[i].fp.off + fp_size - 2;

		if (rd16(data, data_size, (uint32_t)t) != 0xffff)
			return 0;
	}
	return 1;
}

/* Recent tables may lack the pointer block: lay the pointers out the way
 * every table of BDB 155 on has its data (38 + 18 + 10 bytes per panel). */
static int generate_lfp_data_ptrs(struct lg_display *d, uint32_t data_size,
				  struct lfp_ptrs *ptrs)
{
	const int fp_size = 38;
	const int size = fp_size + VBT_DTD_SIZE + VBT_PNP_ID_SIZE;
	int i;

	if (d->vbt.version < 155)
		return 0;
	if (size * 16 > (int)data_size)
		return 0;
	i915_dbg("[drm] i915: generating LFP data table pointers\n");
	mm_memset(ptrs, 0, sizeof(*ptrs));
	ptrs->num_entries = 3;
	for (i = 0; i < 16; i++) {
		ptrs->p[i].fp.off = i * size;
		ptrs->p[i].fp.size = fp_size;
		ptrs->p[i].dvo.off = i * size + fp_size;
		ptrs->p[i].dvo.size = VBT_DTD_SIZE;
		ptrs->p[i].pnp.off = i * size + fp_size + VBT_DTD_SIZE;
		ptrs->p[i].pnp.size = VBT_PNP_ID_SIZE;
	}
	if (16 * (size + VBT_PANEL_NAME_SIZE) <= (int)data_size) {
		ptrs->name.size = VBT_PANEL_NAME_SIZE;
		ptrs->name.off = size * 16;
	}
	return 1;
}

static int get_lfp_data_ptrs(struct lg_display *d, const uint8_t *data, uint32_t data_size,
			     struct lfp_ptrs *ptrs)
{
	uint32_t len = 0, data_off = 0, dummy;
	const uint8_t *b = lg_vbt_block(d, VBT_BDB_LFP_DATA_PTRS, &len);
	int i;

	if (!b)
		return generate_lfp_data_ptrs(d, data_size, ptrs) &&
		       validate_lfp_data_ptrs(ptrs, data, data_size);

	mm_memset(ptrs, 0, sizeof(*ptrs));
	ptrs->num_entries = rd8(b, len, 0);
	for (i = 0; i < 16; i++) {
		lfp_tbl_read(b, len, 1 + i * 9 + 0, &ptrs->p[i].fp);
		lfp_tbl_read(b, len, 1 + i * 9 + 3, &ptrs->p[i].dvo);
		lfp_tbl_read(b, len, 1 + i * 9 + 6, &ptrs->p[i].pnp);
	}
	lfp_tbl_read(b, len, 1 + 16 * 9, &ptrs->name);

	/* the table's offsets count from the start of the BDB */
	data_off = find_section(d, VBT_BDB_LFP_DATA, &dummy);
	for (i = 0; i < 16; i++) {
		if (ptrs->p[i].fp.off < (int)data_off || ptrs->p[i].dvo.off < (int)data_off ||
		    ptrs->p[i].pnp.off < (int)data_off)
			goto bad;
		ptrs->p[i].fp.off -= (int)data_off;
		ptrs->p[i].dvo.off -= (int)data_off;
		ptrs->p[i].pnp.off -= (int)data_off;
	}
	if (ptrs->name.size) {
		if (ptrs->name.off < (int)data_off)
			goto bad;
		ptrs->name.off -= (int)data_off;
	}
	if (validate_lfp_data_ptrs(ptrs, data, data_size))
		return 1;
bad:
	kprintf("[drm] i915: VBT has malformed LFP data table pointers\n");
	return 0;
}

static void parse_lfp_data(struct lg_display *d, int panel_type)
{
	struct lfp_ptrs ptrs;
	uint32_t data_size = 0;
	const uint8_t *data = lg_vbt_block(d, VBT_BDB_LFP_DATA, &data_size);
	const uint8_t *fp;
	uint32_t x_res, y_res, lvds_reg, lvds_val;

	if (!data || !get_lfp_data_ptrs(d, data, data_size, &ptrs))
		return;

	if (fill_detail_timing(data + ptrs.p[panel_type].dvo.off, &d->vbt.lfp_mode,
			       &d->vbt.lfp_width_mm, &d->vbt.lfp_height_mm)) {
		d->vbt.lfp_mode_valid = 1;
		i915_dbg("[drm] i915: VBT panel mode %ux%u@%u clock %u kHz, %ux%u mm\n",
			 d->vbt.lfp_mode.hdisplay, d->vbt.lfp_mode.vdisplay,
			 d->vbt.lfp_mode.vrefresh, d->vbt.lfp_mode.clock, d->vbt.lfp_width_mm,
			 d->vbt.lfp_height_mm);
	}

	/* The LVDS register value the video BIOS programs, when the entry is
	 * for this very resolution and names the LVDS port register. */
	fp = data + ptrs.p[panel_type].fp.off;
	x_res = rd16(fp, (uint32_t)ptrs.p[panel_type].fp.size, 0);
	y_res = rd16(fp, (uint32_t)ptrs.p[panel_type].fp.size, 2);
	lvds_reg = rd32(fp, (uint32_t)ptrs.p[panel_type].fp.size, 4);
	lvds_val = rd32(fp, (uint32_t)ptrs.p[panel_type].fp.size, 8);
	if (d->vbt.lfp_mode_valid && x_res == d->vbt.lfp_mode.hdisplay &&
	    y_res == d->vbt.lfp_mode.vdisplay && (lvds_reg == LVDS || lvds_reg == PCH_LVDS)) {
		d->vbt.bios_lvds_val = lvds_val;
		/* the A3 pair carries the two extra bits of an 8 bpc panel */
		d->vbt.lfp_bpc = (lvds_val & LVDS_A3_POWER_MASK) == LVDS_A3_POWER_UP ? 8 : 6;
		i915_dbg("[drm] i915: VBT initial LVDS value %08x\n", lvds_val);
	}
}

static void parse_lfp_backlight(struct lg_display *d, int panel_type)
{
	uint32_t len = 0;
	const uint8_t *b = lg_vbt_block(d, VBT_BDB_LFP_BACKLIGHT, &len);
	uint32_t entry = 1 + (uint32_t)panel_type * 6;
	uint8_t e0;
	uint16_t level;

	if (!b)
		return;
	if (rd8(b, len, 0) != 6) {
		i915_dbg("[drm] i915: unsupported VBT backlight entry size %u\n", rd8(b, len, 0));
		return;
	}

	e0 = rd8(b, len, entry);
	d->vbt.bl_present = (e0 & 3) == VBT_BACKLIGHT_TYPE_PWM;
	if (!d->vbt.bl_present) {
		i915_dbg("[drm] i915: PWM backlight not present in VBT (type %u)\n", e0 & 3);
		return;
	}

	d->vbt.bl_type = VBT_BL_CONTROL_DISPLAY;
	if (d->vbt.version >= 191)
		d->vbt.bl_type = rd8(b, len, 113 + (uint32_t)panel_type) & 0xf;

	d->vbt.bl_pwm_freq_hz = rd16(b, len, entry + 1);
	d->vbt.bl_active_low = (e0 >> 2) & 1;

	if (d->vbt.version >= 234) {
		uint32_t min_level;
		int scale;

		level = rd16(b, len, 129 + (uint32_t)panel_type * 4);
		min_level = rd16(b, len, 193 + (uint32_t)panel_type * 4);
		if (d->vbt.version >= 236)
			scale = rd8(b, len, 257 + (uint32_t)panel_type) == 16;
		else
			scale = level > 255;
		if (scale)
			min_level = min_level / 255;
		if (min_level > 255) {
			kprintf("[drm] i915: VBT brightness min level > 255\n");
			min_level = 255;
		}
		d->vbt.bl_min_brightness = (uint8_t)min_level;
	} else {
		level = rd8(b, len, 97 + (uint32_t)panel_type);
		d->vbt.bl_min_brightness = rd8(b, len, entry + 3);
	}

	i915_dbg("[drm] i915: VBT backlight PWM %u Hz, active %s, min brightness %u, level %u, "
		 "type %d\n",
		 d->vbt.bl_pwm_freq_hz, d->vbt.bl_active_low ? "low" : "high",
		 d->vbt.bl_min_brightness, level, d->vbt.bl_type);
}

static void parse_sdvo_lvds_data(struct lg_display *d)
{
	uint32_t len = 0, dlen = 0;
	const uint8_t *opts = lg_vbt_block(d, VBT_BDB_SDVO_LVDS_OPTIONS, &len);
	const uint8_t *dtd;
	uint8_t t[VBT_DTD_SIZE];
	int index, i;

	if (!opts)
		return;
	index = rd8(opts, len, 2);
	dtd = lg_vbt_block(d, VBT_BDB_SDVO_LVDS_DTD, &dlen);
	if (!dtd)
		return;
	if (index >= 4) {
		kprintf("[drm] i915: VBT SDVO panel index %d out of range\n", index);
		return;
	}
	for (i = 0; i < VBT_DTD_SIZE; i++)
		t[i] = rd8(dtd, dlen, (uint32_t)(index * VBT_DTD_SIZE + i));
	if (fill_detail_timing(t, &d->vbt.sdvo_lvds_mode, NULL, NULL)) {
		d->vbt.sdvo_lvds_mode_valid = 1;
		i915_dbg("[drm] i915: VBT SDVO LVDS mode %ux%u@%u\n", d->vbt.sdvo_lvds_mode.hdisplay,
			 d->vbt.sdvo_lvds_mode.vdisplay, d->vbt.sdvo_lvds_mode.vrefresh);
	}
}

static void parse_edp(struct lg_display *d, int panel_type)
{
	uint32_t len = 0;
	const uint8_t *b = lg_vbt_block(d, VBT_BDB_EDP, &len);
	uint32_t pt = (uint32_t)panel_type;
	uint8_t link0, link1;

	if (!b)
		return;
	d->vbt.edp_valid = 1;

	switch (panel_bits(rd32(b, len, 160), panel_type, 2)) {
	case 0:
		d->vbt.edp_bpp = 18;
		break;
	case 1:
		d->vbt.edp_bpp = 24;
		break;
	case 2:
		d->vbt.edp_bpp = 30;
		break;
	}

	/* power sequence, in 100 us units */
	d->vbt.edp_t1_t3 = rd16(b, len, pt * 10 + 0);
	d->vbt.edp_t8 = rd16(b, len, pt * 10 + 2);
	d->vbt.edp_t9 = rd16(b, len, pt * 10 + 4);
	d->vbt.edp_t10 = rd16(b, len, pt * 10 + 6);
	d->vbt.edp_t11_t12 = rd16(b, len, pt * 10 + 8);

	link0 = rd8(b, len, 164 + pt * 2);
	link1 = rd8(b, len, 164 + pt * 2 + 1);

	if (d->vbt.version >= 224) {
		d->vbt.edp_rate_khz = (uint32_t)rd16(b, len, 748 + pt * 2) * 20;
	} else {
		switch (link0 & 0xf) {
		case 0:
			d->vbt.edp_rate_khz = 162000;
			break;
		case 1:
			d->vbt.edp_rate_khz = 270000;
			break;
		case 2:
			d->vbt.edp_rate_khz = 540000;
			break;
		default:
			i915_dbg("[drm] i915: VBT has unknown eDP link rate value %u\n",
				 link0 & 0xf);
			break;
		}
	}

	switch (link0 >> 4) {
	case 0:
		d->vbt.edp_lanes = 1;
		break;
	case 1:
		d->vbt.edp_lanes = 2;
		break;
	case 3:
		d->vbt.edp_lanes = 4;
		break;
	default:
		i915_dbg("[drm] i915: VBT has unknown eDP lane count value %u\n", link0 >> 4);
		break;
	}

	/* as DPCD training values: pre-emphasis level << 3, swing level */
	if ((link1 & 0xf) <= 3)
		d->vbt.edp_preemphasis = (link1 & 0xf) << DP_TRAIN_PRE_EMPHASIS_SHIFT;
	else
		i915_dbg("[drm] i915: VBT has unknown eDP pre-emphasis value %u\n", link1 & 0xf);
	if ((link1 >> 4) <= 3)
		d->vbt.edp_vswing = (link1 >> 4) << DP_TRAIN_VOLTAGE_SWING_SHIFT;
	else
		i915_dbg("[drm] i915: VBT has unknown eDP voltage swing value %u\n", link1 >> 4);

	if (d->vbt.version >= 173) {
		uint32_t vswing = (uint32_t)(rd64(b, len, 204) >> (pt * 4)) & 0xf;

		d->vbt.edp_low_vswing = vswing == 0;
	}

	d->vbt.edp_msa_timing_delay = (uint8_t)panel_bits(rd32(b, len, 196), panel_type, 2);

	i915_dbg("[drm] i915: VBT eDP: %d bpp, %u kHz x%d, T1+T3 %u T8 %u T9 %u T10 %u "
		 "T11+T12 %u\n",
		 d->vbt.edp_bpp, d->vbt.edp_rate_khz, d->vbt.edp_lanes, d->vbt.edp_t1_t3,
		 d->vbt.edp_t8, d->vbt.edp_t9, d->vbt.edp_t10, d->vbt.edp_t11_t12);
}

static void parse_panel(struct lg_display *d)
{
	int pt = get_panel_type(d);

	d->vbt.panel_type = pt;
	i915_dbg("[drm] i915: VBT panel type %d\n", pt);
	parse_panel_options(d);
	parse_lfp_data(d, pt);
	parse_lfp_backlight(d, pt);
	parse_sdvo_lvds_data(d);
	parse_edp(d, pt);
}

/* ---- defaults ----------------------------------------------------------------- */

static void init_vbt_defaults(struct lg_display *d)
{
	d->vbt.crt_ddc_pin = LG_GMBUS_PIN_VGADDC;
	d->vbt.int_tv_support = 1;
	d->vbt.int_crt_support = 1;
	d->vbt.int_lvds_support = 1;
	/* SSC on; the GMCH parts at their alternate frequency, the PCH
	 * parts at 120 MHz */
	d->vbt.lvds_use_ssc = 1;
	d->vbt.lvds_ssc_freq_khz = ssc_frequency(d, d->pch == LG_PCH_NONE);
	d->vbt.panel_type = -1;
	/* a panel has a backlight, and dithers */
	d->vbt.bl_present = 1;
	d->vbt.bl_type = VBT_BL_CONTROL_DISPLAY;
	d->vbt.lvds_dither = 1;
	i915_dbg("[drm] i915: VBT default: SSC at %u kHz\n", d->vbt.lvds_ssc_freq_khz);
}

/* No table at all: Cherryview, whose port detection needs the table,
 * gets a DP/HDMI child device for each of its ports. */
static void init_vbt_missing_defaults(struct lg_display *d)
{
	int port;

	if (!d->is_chv)
		return;
	for (port = LG_PORT_B; port <= LG_PORT_D; port++) {
		struct lg_vbt_child *c;

		if (d->vbt.nchild >= LG_VBT_MAX_CHILDREN)
			break;
		c = &d->vbt.child[d->vbt.nchild];
		mm_memset(c, 0, sizeof(*c));
		c->dvo_port = (uint8_t)(VBT_DVO_PORT_HDMIA + port);
		c->device_type = VBT_DEVICE_TYPE_TMDS_DVI_SIGNALING |
				 VBT_DEVICE_TYPE_DISPLAYPORT_OUTPUT;
		c->aux_ch = 0xff;
		c->hdmi_level_shift = 0xff;
		c->port = port;
		child_update_caps(c);
		g_addin[d->vbt.nchild] = 0;
		d->vbt.nchild++;
		i915_dbg("[drm] i915: default VBT child device type 0x%04x on port %c\n",
			 c->device_type, 'A' + port);
	}
	/* past the baseline version checks */
	d->vbt.version = 155;
}

/* ---- entry points --------------------------------------------------------------- */

int lg_vbt_init(struct lg_display *d)
{
	const struct intel_opregion *op = &d->i915->display.opregion;
	const uint8_t *src = NULL;
	uint32_t size = 0;
	const char *where = NULL;

	lg_vbt_fini(d);
	mm_memset(&d->vbt, 0, sizeof(d->vbt));
	mm_memset(g_addin, 0, sizeof(g_addin));
	g_vbt_from_opregion = 0;
	init_vbt_defaults(d);

	if (op->vbt && op->vbt_size && vbt_is_valid(op->vbt, op->vbt_size)) {
		src = op->vbt;
		size = rd16(src, op->vbt_size, VBT_HDR_VBT_SIZE);
		where = "OpRegion";
		g_vbt_from_opregion = 1;
	} else {
		src = rom_get_vbt(&size);
		if (src)
			where = "video BIOS";
	}

	if (src) {
		d->vbt.raw = kalloc(size);
		if (d->vbt.raw) {
			mm_memcpy(d->vbt.raw, src, size);
			d->vbt.raw_len = size;
		} else {
			kprintf("[drm] i915: no memory for the VBT\n");
			src = NULL;
		}
	}

	if (src) {
		uint32_t bdb_off = rd32(d->vbt.raw, size, VBT_HDR_BDB_OFFSET);

		d->vbt.valid = 1;
		d->vbt.version = rd16(d->vbt.raw, size, bdb_off + VBT_BDB_VERSION);
		kprintf("[drm] i915: VBT from the %s, BDB version %u\n", where, d->vbt.version);

		parse_general_features(d);
		parse_general_definitions(d);
		parse_driver_features(d);
		parse_panel(d);
	} else {
		kprintf("[drm] i915: failed to find the video BIOS tables (VBT)\n");
		init_vbt_missing_defaults(d);
	}

	parse_sdvo_device_mapping(d);
	parse_ddi_ports(d);
	return 0;
}

void lg_vbt_fini(struct lg_display *d)
{
	if (d->vbt.raw)
		kfree(d->vbt.raw);
	d->vbt.raw = NULL;
	d->vbt.raw_len = 0;
	d->vbt.valid = 0;
}

const struct lg_vbt_child *lg_vbt_child_for_port(struct lg_display *d, int port)
{
	int i;

	for (i = 0; i < d->vbt.nchild; i++)
		if (d->vbt.child[i].port == port)
			return &d->vbt.child[i];
	return NULL;
}

int lg_vbt_port_present(struct lg_display *d, int port)
{
	/* before G4X the table does not describe the ports */
	if (!has_ddi_port_info(d))
		return 1;
	return lg_vbt_child_for_port(d, port) != NULL;
}

int lg_vbt_port_is_edp(struct lg_display *d, int port)
{
	const struct lg_vbt_child *c;

	/* no eDP before Ironlake, whatever the table says */
	if (d->ver < 5)
		return 0;
	/* port A of Ironlake to Ivy Bridge is the CPU eDP port (whether it
	 * is there at all is the fuse strap's business) */
	if (port == LG_PORT_A && !d->is_vlv && !d->is_chv)
		return 1;
	c = lg_vbt_child_for_port(d, port);
	return c && c->supports_edp;
}

int lg_vbt_lvds_present(struct lg_display *d, uint8_t *i2c_pin)
{
	int i;

	if (!d->vbt.int_lvds_support)
		return 0;
	if (!d->vbt.nchild)
		return 1;
	for (i = 0; i < d->vbt.nchild; i++) {
		const struct lg_vbt_child *c = &d->vbt.child[i];

		/* the new and the old codes both appear */
		if (c->device_type != VBT_DEVICE_TYPE_INT_LFP &&
		    c->device_type != VBT_DEVICE_TYPE_LFP)
			continue;
		if (i2c_pin && lg_gmbus_pin_valid(d, c->i2c_pin))
			*i2c_pin = c->i2c_pin;
		/* An LVDS needs add-in data from the video BIOS, so an add-in
		 * offset is a good sign it is really there. */
		if (g_addin[i])
			return 1;
		/* Some tables instantiate it without; trust those the
		 * platform firmware put in the OpRegion. */
		return g_vbt_from_opregion;
	}
	return 0;
}

int lg_vbt_tv_present(struct lg_display *d)
{
	int i;

	if (!d->vbt.int_tv_support)
		return 0;
	if (!d->vbt.nchild)
		return 1;
	for (i = 0; i < d->vbt.nchild; i++) {
		switch (d->vbt.child[i].device_type) {
		case VBT_DEVICE_TYPE_INT_TV:
		case VBT_DEVICE_TYPE_TV:
		case VBT_DEVICE_TYPE_TV_SVIDEO_COMPOSITE:
			break;
		default:
			continue;
		}
		/* only one the video BIOS set up counts */
		if (g_addin[i])
			return 1;
	}
	return 0;
}

int lg_vbt_dsi_present(struct lg_display *d, int *port)
{
	int i;

	for (i = 0; i < d->vbt.nchild; i++) {
		const struct lg_vbt_child *c = &d->vbt.child[i];
		int p;

		if (!(c->device_type & VBT_DEVICE_TYPE_MIPI_OUTPUT))
			continue;
		if (c->dvo_port == VBT_DVO_PORT_MIPIA) {
			p = 0;
		} else if (c->dvo_port == VBT_DVO_PORT_MIPIC) {
			p = 2;
		} else {
			i915_dbg("[drm] i915: VBT has unsupported DSI port %c\n",
				 'A' + c->dvo_port - VBT_DVO_PORT_MIPIA);
			continue;
		}
		if (port)
			*port = p;
		return 1;
	}
	return 0;
}
