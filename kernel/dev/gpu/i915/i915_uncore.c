// LikeOS -- register access and forcewake for Intel graphics.
//
// The GT's register file is not always powered: the hardware puts render,
// media and GT-common units to sleep on their own, and a register read
// from a sleeping unit returns nonsense (or hangs on some steppings).
// Forcewake is the request to keep a unit awake -- write the request bit,
// wait for the acknowledge -- and every GT register access below takes
// the domains its offset belongs to, refcounted so nested users pay once.
// Display registers need none.
//
// From Gen12 on, which domain an offset belongs to is a table per
// platform, as the hardware documentation lists it, together with the
// registers a write needs no forcewake for: the "shadowed" ones, which
// the hardware keeps a copy of and wakes the unit for on its own.
// Meteor Lake's media GT has its own domains and its own table; its
// registers below 0x40000 are the primary GT's plus 0x380000, its
// engines and its security controller keep their offsets.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2013-2023 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/i915_xe2_reg.h>
#include <kernel/dev/gpu/i915/i915_legacy.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

#define FW_ACK_TIMEOUT_US 50000

static inline volatile uint32_t *reg32(struct i915_device *i915, uint32_t reg)
{
	return (volatile uint32_t *)(i915->mmio_virt + reg);
}

/* Wa_15015404425: on Lunar Lake and Panther Lake a register read can
 * overtake writes the graphics unit has not finished; four writes of a
 * dummy register ahead of every read make it wait for them. */
static inline void mmio_flush_pending_writes(struct i915_device *i915)
{
	if (!i915->mmio_read_flush)
		return;
	for (int i = 0; i < 4; i++)
		*reg32(i915, XE2_MMIO_FLUSH_DUMMY_REG) = 0;
}

uint32_t i915_read32_fw(struct i915_device *i915, uint32_t reg)
{
	mmio_flush_pending_writes(i915);
	return *reg32(i915, reg);
}

void i915_write32_fw(struct i915_device *i915, uint32_t reg, uint32_t val)
{
	*reg32(i915, reg) = val;
}

/* ---- which domains a register lives in ------------------------------------ */

/* Gen12 and later: the tables.  Every range is inclusive, the ranges of a
 * table follow each other without a gap. */
struct fw_range {
	uint32_t start, end;
	uint32_t domains;
};

struct fw_shadow {
	uint32_t start, end;
};

#define FW_R I915_FW_RENDER
#define FW_GT I915_FW_GT
#define FW_MGT I915_FW_MEDIA_GT
#define FW_GSC I915_FW_GSC
#define FW_VD0 I915_FW_VDBOX0
#define FW_VD1 I915_FW_VDBOX1
#define FW_VD2 I915_FW_VDBOX2
#define FW_VD3 I915_FW_VDBOX3
#define FW_VE0 I915_FW_VEBOX0
#define FW_VE1 I915_FW_VEBOX1
#define FW_NONE 0 /* units no part driven here has */
#define FW_ALL 0x80000000u /* every domain of the GT */

static const struct fw_range gen12_fw_ranges[] = {
	{ 0x000000, 0x001fff, 0 },
	{ 0x002000, 0x0026ff, FW_R },
	{ 0x002700, 0x0027ff, FW_GT },
	{ 0x002800, 0x002aff, FW_R },
	{ 0x002b00, 0x002fff, FW_GT },
	{ 0x003000, 0x003fff, FW_R },
	{ 0x004000, 0x0051ff, FW_GT },
	{ 0x005200, 0x007fff, FW_R },
	{ 0x008000, 0x00813f, FW_GT },
	{ 0x008140, 0x00815f, FW_R },
	{ 0x008160, 0x0081ff, 0 },
	{ 0x008200, 0x0082ff, FW_GT },
	{ 0x008300, 0x0084ff, FW_R },
	{ 0x008500, 0x0094cf, FW_GT },
	{ 0x0094d0, 0x00955f, FW_R },
	{ 0x009560, 0x0097ff, 0 },
	{ 0x009800, 0x00afff, FW_GT },
	{ 0x00b000, 0x00b3ff, FW_R },
	{ 0x00b400, 0x00cfff, FW_GT },
	{ 0x00d000, 0x00d7ff, 0 },
	{ 0x00d800, 0x00d8ff, FW_R },
	{ 0x00d900, 0x00dbff, FW_GT },
	{ 0x00dc00, 0x00efff, FW_R },
	{ 0x00f000, 0x0147ff, FW_GT },
	{ 0x014800, 0x01ffff, FW_R },
	{ 0x020000, 0x020fff, FW_VD0 },
	{ 0x021000, 0x021fff, FW_VD2 },
	{ 0x022000, 0x023fff, FW_GT },
	{ 0x024000, 0x02417f, 0 },
	{ 0x024180, 0x0249ff, FW_GT },
	{ 0x024a00, 0x0251ff, FW_R },
	{ 0x025200, 0x0255ff, FW_GT },
	{ 0x025600, 0x02567f, FW_VD0 },
	{ 0x025680, 0x0259ff, FW_VD2 },
	{ 0x025a00, 0x025a7f, FW_VD0 },
	{ 0x025a80, 0x02ffff, FW_VD2 },
	{ 0x030000, 0x03ffff, FW_GT },
	{ 0x040000, 0x1bffff, 0 },
	{ 0x1c0000, 0x1c3fff, FW_VD0 },
	{ 0x1c4000, 0x1c7fff, 0 },
	{ 0x1c8000, 0x1cbfff, FW_VE0 },
	{ 0x1cc000, 0x1cffff, FW_VD0 },
	{ 0x1d0000, 0x1d3fff, FW_VD2 },
};

static const struct fw_range dg2_fw_ranges[] = {
	{ 0x000000, 0x001fff, 0 },
	{ 0x002000, 0x0026ff, FW_R },
	{ 0x002700, 0x004aff, FW_GT },
	{ 0x004b00, 0x0051ff, 0 },
	{ 0x005200, 0x007fff, FW_R },
	{ 0x008000, 0x00813f, FW_GT },
	{ 0x008140, 0x00815f, FW_R },
	{ 0x008160, 0x0081ff, 0 },
	{ 0x008200, 0x0082ff, FW_GT },
	{ 0x008300, 0x0084ff, FW_R },
	{ 0x008500, 0x008cff, FW_GT },
	{ 0x008d00, 0x008fff, FW_R },
	{ 0x009000, 0x0094cf, FW_GT },
	{ 0x0094d0, 0x00955f, FW_R },
	{ 0x009560, 0x00967f, 0 },
	{ 0x009680, 0x0097ff, FW_R },
	{ 0x009800, 0x00cfff, FW_GT },
	{ 0x00d000, 0x00d7ff, 0 },
	{ 0x00d800, 0x00d87f, FW_R },
	{ 0x00d880, 0x00dbff, FW_GT },
	{ 0x00dc00, 0x00dcff, FW_R },
	{ 0x00dd00, 0x00de7f, FW_GT },
	{ 0x00de80, 0x00e8ff, FW_R },
	{ 0x00e900, 0x00ffff, FW_GT },
	{ 0x010000, 0x012fff, 0 },
	{ 0x013000, 0x0131ff, FW_VD0 },
	{ 0x013200, 0x0147ff, FW_VD2 },
	{ 0x014800, 0x014fff, FW_R },
	{ 0x015000, 0x016dff, FW_GT },
	{ 0x016e00, 0x021fff, FW_R },
	{ 0x022000, 0x023fff, FW_GT },
	{ 0x024000, 0x02417f, 0 },
	{ 0x024180, 0x0249ff, FW_GT },
	{ 0x024a00, 0x0251ff, FW_R },
	{ 0x025200, 0x025fff, FW_GT },
	{ 0x026000, 0x02ffff, FW_R },
	{ 0x030000, 0x03ffff, FW_GT },
	{ 0x040000, 0x1bffff, 0 },
	{ 0x1c0000, 0x1c3fff, FW_VD0 },
	{ 0x1c4000, 0x1c7fff, FW_VD1 },
	{ 0x1c8000, 0x1cbfff, FW_VE0 },
	{ 0x1cc000, 0x1ccfff, FW_VD0 },
	{ 0x1cd000, 0x1cdfff, FW_VD2 },
	{ 0x1ce000, 0x1cefff, FW_NONE },
	{ 0x1cf000, 0x1cffff, FW_NONE },
	{ 0x1d0000, 0x1d3fff, FW_VD2 },
	{ 0x1d4000, 0x1d7fff, FW_VD3 },
	{ 0x1d8000, 0x1dffff, FW_VE1 },
	{ 0x1e0000, 0x1e3fff, FW_NONE },
	{ 0x1e4000, 0x1e7fff, FW_NONE },
	{ 0x1e8000, 0x1effff, FW_NONE },
	{ 0x1f0000, 0x1f3fff, FW_NONE },
	{ 0x1f4000, 0x1f7fff, FW_NONE },
	{ 0x1f8000, 0x1fa0ff, FW_NONE },
};

static const struct fw_range mtl_fw_ranges[] = {
	{ 0x000000, 0x000aff, 0 },
	{ 0x000b00, 0x000bff, FW_GT },
	{ 0x000c00, 0x000fff, 0 },
	{ 0x001000, 0x001fff, FW_GT },
	{ 0x002000, 0x0026ff, FW_R },
	{ 0x002700, 0x002fff, FW_GT },
	{ 0x003000, 0x003fff, FW_R },
	{ 0x004000, 0x0051ff, FW_GT },
	{ 0x005200, 0x007fff, FW_R },
	{ 0x008000, 0x00813f, FW_GT },
	{ 0x008140, 0x00817f, FW_R },
	{ 0x008180, 0x0081ff, 0 },
	{ 0x008200, 0x0094cf, FW_GT },
	{ 0x0094d0, 0x00955f, FW_R },
	{ 0x009560, 0x00967f, 0 },
	{ 0x009680, 0x0097ff, FW_R },
	{ 0x009800, 0x00cfff, FW_GT },
	{ 0x00d000, 0x00d7ff, 0 },
	{ 0x00d800, 0x00d87f, FW_R },
	{ 0x00d880, 0x00dbff, FW_GT },
	{ 0x00dc00, 0x00dcff, FW_R },
	{ 0x00dd00, 0x00de7f, FW_GT },
	{ 0x00de80, 0x00e8ff, FW_R },
	{ 0x00e900, 0x00e9ff, FW_GT },
	{ 0x00ea00, 0x0147ff, 0 },
	{ 0x014800, 0x019fff, FW_GT },
	{ 0x01a000, 0x021fff, FW_R },
	{ 0x022000, 0x023fff, FW_GT },
	{ 0x024000, 0x02ffff, 0 },
	{ 0x030000, 0x03ffff, FW_GT },
	{ 0x040000, 0x1901ef, 0 },
	{ 0x1901f0, 0x1901f3, FW_GT },
};

static const struct fw_range xelpmp_fw_ranges[] = {
	{ 0x000000, 0x115fff, 0 },
	{ 0x116000, 0x11ffff, FW_GSC },
	{ 0x120000, 0x1bffff, 0 },
	{ 0x1c0000, 0x1c7fff, FW_VD0 },
	{ 0x1c8000, 0x1cbfff, FW_VE0 },
	{ 0x1cc000, 0x1cffff, FW_VD0 },
	{ 0x1d0000, 0x1d7fff, FW_VD2 },
	{ 0x1d8000, 0x1da0ff, FW_VE1 },
	{ 0x1da100, 0x380aff, 0 },
	{ 0x380b00, 0x380bff, FW_MGT },
	{ 0x380c00, 0x380fff, 0 },
	{ 0x381000, 0x38817f, FW_MGT },
	{ 0x388180, 0x3882ff, 0 },
	{ 0x388300, 0x38955f, FW_MGT },
	{ 0x389560, 0x389fff, 0 },
	{ 0x38a000, 0x38cfff, FW_MGT },
	{ 0x38d000, 0x38d11f, 0 },
	{ 0x38d120, 0x391fff, FW_MGT },
	{ 0x392000, 0x392fff, 0 },
	{ 0x393000, 0x3931ff, FW_MGT },
	{ 0x393200, 0x39323f, FW_ALL },
	{ 0x393240, 0x3933ff, FW_MGT },
	{ 0x393400, 0x3934ff, FW_ALL },
	{ 0x393500, 0x393c7f, 0 },
	{ 0x393c80, 0x393dff, FW_MGT },
};

static const struct fw_shadow gen12_shadowed_regs[] = {
	{ 0x002030, 0x002030 },
	{ 0x002510, 0x002550 },
	{ 0x00a008, 0x00a00c },
	{ 0x00a188, 0x00a188 },
	{ 0x00a278, 0x00a278 },
	{ 0x00a540, 0x00a56c },
	{ 0x00c4c8, 0x00c4c8 },
	{ 0x00c4d4, 0x00c4d4 },
	{ 0x00c600, 0x00c600 },
	{ 0x022030, 0x022030 },
	{ 0x022510, 0x022550 },
	{ 0x1c0030, 0x1c0030 },
	{ 0x1c0510, 0x1c0550 },
	{ 0x1c4030, 0x1c4030 },
	{ 0x1c4510, 0x1c4550 },
	{ 0x1c8030, 0x1c8030 },
	{ 0x1c8510, 0x1c8550 },
	{ 0x1d0030, 0x1d0030 },
	{ 0x1d0510, 0x1d0550 },
	{ 0x1d4030, 0x1d4030 },
	{ 0x1d4510, 0x1d4550 },
	{ 0x1d8030, 0x1d8030 },
	{ 0x1d8510, 0x1d8550 },
	{ 0x1e0030, 0x1e0030 },
	{ 0x1e0510, 0x1e0550 },
	{ 0x1e4030, 0x1e4030 },
	{ 0x1e4510, 0x1e4550 },
	{ 0x1e8030, 0x1e8030 },
	{ 0x1e8510, 0x1e8550 },
	{ 0x1f0030, 0x1f0030 },
	{ 0x1f0510, 0x1f0550 },
	{ 0x1f4030, 0x1f4030 },
	{ 0x1f4510, 0x1f4550 },
	{ 0x1f8030, 0x1f8030 },
	{ 0x1f8510, 0x1f8550 },
};

static const struct fw_shadow dg2_shadowed_regs[] = {
	{ 0x002030, 0x002030 },
	{ 0x002510, 0x002550 },
	{ 0x00a008, 0x00a00c },
	{ 0x00a188, 0x00a188 },
	{ 0x00a278, 0x00a278 },
	{ 0x00a540, 0x00a56c },
	{ 0x00c4c8, 0x00c4c8 },
	{ 0x00c4e0, 0x00c4e0 },
	{ 0x00c600, 0x00c600 },
	{ 0x00c658, 0x00c658 },
	{ 0x022030, 0x022030 },
	{ 0x022510, 0x022550 },
	{ 0x1c0030, 0x1c0030 },
	{ 0x1c0510, 0x1c0550 },
	{ 0x1c4030, 0x1c4030 },
	{ 0x1c4510, 0x1c4550 },
	{ 0x1c8030, 0x1c8030 },
	{ 0x1c8510, 0x1c8550 },
	{ 0x1d0030, 0x1d0030 },
	{ 0x1d0510, 0x1d0550 },
	{ 0x1d4030, 0x1d4030 },
	{ 0x1d4510, 0x1d4550 },
	{ 0x1d8030, 0x1d8030 },
	{ 0x1d8510, 0x1d8550 },
	{ 0x1e0030, 0x1e0030 },
	{ 0x1e0510, 0x1e0550 },
	{ 0x1e4030, 0x1e4030 },
	{ 0x1e4510, 0x1e4550 },
	{ 0x1e8030, 0x1e8030 },
	{ 0x1e8510, 0x1e8550 },
	{ 0x1f0030, 0x1f0030 },
	{ 0x1f0510, 0x1f0550 },
	{ 0x1f4030, 0x1f4030 },
	{ 0x1f4510, 0x1f4550 },
	{ 0x1f8030, 0x1f8030 },
	{ 0x1f8510, 0x1f8550 },
};

static const struct fw_shadow mtl_shadowed_regs[] = {
	{ 0x002030, 0x002030 },
	{ 0x002510, 0x002550 },
	{ 0x00a008, 0x00a00c },
	{ 0x00a188, 0x00a188 },
	{ 0x00a278, 0x00a278 },
	{ 0x00a540, 0x00a56c },
	{ 0x00c050, 0x00c050 },
	{ 0x00c340, 0x00c340 },
	{ 0x00c4c8, 0x00c4c8 },
	{ 0x00c4e0, 0x00c4e0 },
	{ 0x00c600, 0x00c600 },
	{ 0x00c658, 0x00c658 },
	{ 0x00cfd4, 0x00cfdc },
	{ 0x022030, 0x022030 },
	{ 0x022510, 0x022550 },
};

static const struct fw_shadow xelpmp_shadowed_regs[] = {
	{ 0x1c0030, 0x1c0030 },
	{ 0x1c0510, 0x1c0550 },
	{ 0x1c8030, 0x1c8030 },
	{ 0x1c8510, 0x1c8550 },
	{ 0x1d0030, 0x1d0030 },
	{ 0x1d0510, 0x1d0550 },
	{ 0x38a008, 0x38a00c },
	{ 0x38a188, 0x38a188 },
	{ 0x38a278, 0x38a278 },
	{ 0x38a540, 0x38a56c },
	{ 0x38a618, 0x38a618 },
	{ 0x38c050, 0x38c050 },
	{ 0x38c340, 0x38c340 },
	{ 0x38c4c8, 0x38c4c8 },
	{ 0x38c4e0, 0x38c4e4 },
	{ 0x38c600, 0x38c600 },
	{ 0x38c658, 0x38c658 },
	{ 0x38cfd4, 0x38cfdc },
};

/* Cherryview: which of its two domains a register needs; what the
 * table does not name needs none. */
#define FW_M I915_FW_MEDIA
static const struct fw_range chv_fw_ranges[] = {
	{ 0x02000, 0x03fff, FW_R },
	{ 0x04000, 0x04fff, FW_R | FW_M },
	{ 0x05200, 0x07fff, FW_R },
	{ 0x08000, 0x082ff, FW_R | FW_M },
	{ 0x08300, 0x084ff, FW_R },
	{ 0x08500, 0x085ff, FW_R | FW_M },
	{ 0x08800, 0x088ff, FW_M },
	{ 0x09000, 0x0afff, FW_R | FW_M },
	{ 0x0b000, 0x0b47f, FW_R },
	{ 0x0d000, 0x0d7ff, FW_M },
	{ 0x0e000, 0x0e7ff, FW_R },
	{ 0x0f000, 0x0ffff, FW_R | FW_M },
	{ 0x12000, 0x13fff, FW_M },
	{ 0x1a000, 0x1bfff, FW_M },
	{ 0x1e800, 0x1e9ff, FW_M },
	{ 0x30000, 0x37fff, FW_M },
};

static uint32_t fw_table_lookup(const struct fw_range *t, unsigned n, uint32_t reg)
{
	unsigned lo = 0, hi = n;

	while (lo < hi) {
		unsigned mid = (lo + hi) / 2;
		if (reg < t[mid].start)
			hi = mid;
		else if (reg > t[mid].end)
			lo = mid + 1;
		else
			return t[mid].domains;
	}
	return 0;
}

static int fw_shadowed(const struct fw_shadow *t, unsigned n, uint32_t reg)
{
	unsigned lo = 0, hi = n;

	while (lo < hi) {
		unsigned mid = (lo + hi) / 2;
		if (reg < t[mid].start)
			hi = mid;
		else if (reg > t[mid].end)
			lo = mid + 1;
		else
			return 1;
	}
	return 0;
}

#define MEDIA_GT_DOMAINS (I915_FW_MEDIA_GT | I915_FW_VDBOX0 | I915_FW_VDBOX1 | I915_FW_VDBOX2 | \
			  I915_FW_VDBOX3 | I915_FW_VEBOX0 | I915_FW_VEBOX1 | I915_FW_GSC)
#define MEDIA_ENGINE_DOMAINS (I915_FW_VDBOX0 | I915_FW_VDBOX1 | I915_FW_VDBOX2 | I915_FW_VDBOX3 | \
			      I915_FW_VEBOX0 | I915_FW_VEBOX1)

/* Xe2 on: no table.  The GT is woken as a whole for its registers, as the
 * source of these parts does, which holds every domain a GT has around
 * its accesses rather than looking a register up: the primary GT's
 * domains for everything below 0x40000 (and its service copy engine
 * above the media GT), the media GT's for its relocated registers, its
 * engines and its security controller; the display, the interrupt
 * hierarchy and the rest of the SoC need none -- except the GuC's
 * doorbell, which is the primary GT's. */
static uint32_t xe2_fw_domains(struct i915_device *i915, uint32_t reg)
{
	uint32_t primary = i915->fw_present & ~MEDIA_GT_DOMAINS;
	uint32_t media = i915->fw_present & MEDIA_GT_DOMAINS;

	if (reg < 0x40000)
		return primary;
	if (reg >= 0x1901f0 && reg <= 0x1901f3)
		return i915->fw_present & I915_FW_GT;
	if (i915->has_media_gt) {
		if (reg >= 0x116000 && reg < 0x120000)
			return media & (I915_FW_MEDIA_GT | I915_FW_GSC);
		if (reg >= 0x1c0000 && reg < 0x200000)
			return media & (I915_FW_MEDIA_GT | MEDIA_ENGINE_DOMAINS);
		if (reg >= I915_MEDIA_GT_BASE && reg < I915_MEDIA_GT_BASE + 0x40000)
			return media & I915_FW_MEDIA_GT;
	} else if (reg >= 0x1c0000 && reg < 0x200000) {
		return primary;
	}
	if (reg >= 0x3c0000 && reg < 0x400000)
		return primary;
	return 0;
}

static uint32_t gen12_fw_domains(struct i915_device *i915, uint32_t reg, int write)
{
	const struct fw_range *t;
	const struct fw_shadow *sh;
	unsigned n, nsh;
	uint32_t all = i915->fw_present;

	/* the display engine and the rest of the SoC between need none */
	if (reg >= 0x40000 && reg < 0x116000)
		return 0;
	if (i915->gt_ip >= I915_IP(20, 0))
		return xe2_fw_domains(i915, reg);
	if (i915->gt_ip >= I915_IP(12, 70)) {
		int media = i915->has_media_gt &&
			    ((reg >= 0x116000 && reg < 0x200000) ||
			     (reg >= I915_MEDIA_GT_BASE && reg < I915_MEDIA_GT_BASE + 0x40000));
		if (media) {
			t = xelpmp_fw_ranges;
			n = sizeof(xelpmp_fw_ranges) / sizeof(xelpmp_fw_ranges[0]);
			sh = xelpmp_shadowed_regs;
			nsh = sizeof(xelpmp_shadowed_regs) / sizeof(xelpmp_shadowed_regs[0]);
			all &= MEDIA_GT_DOMAINS;
		} else {
			t = mtl_fw_ranges;
			n = sizeof(mtl_fw_ranges) / sizeof(mtl_fw_ranges[0]);
			sh = mtl_shadowed_regs;
			nsh = sizeof(mtl_shadowed_regs) / sizeof(mtl_shadowed_regs[0]);
			all &= ~MEDIA_GT_DOMAINS;
		}
	} else if (i915->gt_ip >= I915_IP(12, 55)) {
		t = dg2_fw_ranges;
		n = sizeof(dg2_fw_ranges) / sizeof(dg2_fw_ranges[0]);
		sh = dg2_shadowed_regs;
		nsh = sizeof(dg2_shadowed_regs) / sizeof(dg2_shadowed_regs[0]);
	} else {
		t = gen12_fw_ranges;
		n = sizeof(gen12_fw_ranges) / sizeof(gen12_fw_ranges[0]);
		sh = gen12_shadowed_regs;
		nsh = sizeof(gen12_shadowed_regs) / sizeof(gen12_shadowed_regs[0]);
	}
	if (write && fw_shadowed(sh, nsh, reg))
		return 0;
	uint32_t d = fw_table_lookup(t, n, reg);
	if (d & FW_ALL)
		return all;
	return d & i915->fw_present;
}

/* Ranges are the GT's own map; anything display-side (0x40000 and up on
 * the north side, the PCH from 0xc0000) needs no forcewake.  Unknown GT
 * ranges take every domain, which is slower but never wrong. */
uint32_t i915_fw_domains_for(struct i915_device *i915, uint32_t reg)
{
	const struct intel_device_info *info = i915->info;

	if (i915_is_legacy(i915))
		return i915_legacy_fw_domains_for(i915, reg, 0);
	if (info->gen_x10 >= 120)
		return gen12_fw_domains(i915, reg, 0);
	if (info->platform == I915_PLATFORM_CHERRYVIEW)
		return fw_table_lookup(chv_fw_ranges, sizeof(chv_fw_ranges) / sizeof(chv_fw_ranges[0]),
				       reg) &
		       i915->fw_present;
	if (reg >= 0x40000 && reg < 0x100000)
		return 0; /* display engine */
	if (reg >= 0xc0000 && reg < 0xe0000)
		return 0; /* PCH display */
	if (reg >= 0x101000 && reg < 0x102000)
		return 0; /* GTT flush control */
	if (info->gen == 8)
		return I915_FW_RENDER; /* the single multi-thread domain */
	if (info->gen >= 11) {
		if (reg >= 0x1c0000 && reg < 0x1c4000)
			return I915_FW_VDBOX0;
		if (reg >= 0x1c4000 && reg < 0x1c8000)
			return I915_FW_VDBOX1;
		if (reg >= 0x1c8000 && reg < 0x1cc000)
			return I915_FW_VEBOX0;
		if (reg >= 0x1d0000 && reg < 0x1d4000)
			return I915_FW_VDBOX2;
		if (reg >= 0x1d4000 && reg < 0x1d8000)
			return I915_FW_VDBOX3;
		if (reg >= 0x1d8000 && reg < 0x1dc000)
			return I915_FW_VEBOX1;
		if (reg >= 0x190000 && reg < 0x1a0000)
			return 0; /* interrupt hierarchy: always awake */
	}
	/* Gen9+: render 0x2000-0x4000 (engine + rc6), 0x5000-0x8000,
	 * 0x8000-0xb000 GT, 0xb000-0xc000 render, 0xc000-0xd000 GT,
	 * 0xd000-0xd800 ack registers (none), 0xe000-0x24000 render/GT,
	 * media engines at 0x12000/0x1a000 (Gen9), 0x22000 blitter. */
	if (reg >= 0x2000 && reg < 0x4000)
		return I915_FW_RENDER;
	if (reg >= 0x4000 && reg < 0x5000)
		return I915_FW_GT;
	if (reg >= 0x5000 && reg < 0x8000)
		return I915_FW_RENDER;
	if (reg >= 0x8000 && reg < 0x8300)
		return I915_FW_GT;
	if (reg >= 0x8300 && reg < 0x8500)
		return I915_FW_RENDER;
	if (reg >= 0x8500 && reg < 0xb000)
		return I915_FW_GT;
	if (reg >= 0xb000 && reg < 0xb480)
		return I915_FW_RENDER;
	if (reg >= 0xb480 && reg < 0xc000)
		return I915_FW_GT;
	if (reg >= 0xc000 && reg < 0xd000)
		return I915_FW_GT;
	if (reg >= 0xd000 && reg < 0xd800)
		return 0; /* forcewake acks themselves */
	if (reg >= 0xd800 && reg < 0xe000)
		return I915_FW_GT;
	if (reg >= 0xe000 && reg < 0x12000)
		return I915_FW_RENDER;
	if (info->gen < 11 && reg >= 0x12000 && reg < 0x14000)
		return I915_FW_MEDIA; /* VCS0 */
	if (reg >= 0x14000 && reg < 0x1a000)
		return I915_FW_GT;
	if (info->gen < 11 && reg >= 0x1a000 && reg < 0x1ea00)
		return I915_FW_MEDIA; /* VECS0, and VCS1 on the GT3 parts */
	if (info->gen >= 11 && reg >= 0x1c000 && reg < 0x1ea00)
		return I915_FW_GT;
	if (reg >= 0x1ea00 && reg < 0x22000)
		return I915_FW_GT;
	if (reg >= 0x22000 && reg < 0x24000)
		return I915_FW_GT; /* BCS0 */
	if (reg >= 0x24000 && reg < 0x40000)
		return I915_FW_GT;
	return i915->fw_present;
}

/* ---- the domains ----------------------------------------------------------- */

static void fw_domain_set(struct i915_device *i915, struct i915_fw_domain *d,
			  int on)
{
	i915_write32_fw(i915, d->reg_set,
			on ? I915_MASKED_ENABLE(FORCEWAKE_KERNEL) :
			     I915_MASKED_DISABLE(FORCEWAKE_KERNEL));
	/* posting read on a register that needs no forcewake */
	(void)i915_read32_fw(i915, d->reg_ack);
}

static int fw_domain_wait_ack(struct i915_device *i915,
			      struct i915_fw_domain *d, int on)
{
	uint32_t want = on ? FORCEWAKE_KERNEL : 0;

	for (uint32_t t = 0; t < FW_ACK_TIMEOUT_US; t += 10) {
		if ((i915_read32_fw(i915, d->reg_ack) & FORCEWAKE_KERNEL) == want)
			return 0;
		lapic_delay_us(10);
	}
	d->timeouts++;
	if (d->timeouts == 1)
		kprintf("[drm] i915: forcewake domain %x: no %s acknowledge (ack=%08x)\n",
			d->mask, on ? "wake" : "sleep",
			i915_read32_fw(i915, d->reg_ack));
	return -ETIMEDOUT;
}

/* Caller holds fw_lock. */
static void fw_domains_get_locked(struct i915_device *i915, uint32_t mask)
{
	for (int i = 0; i < I915_FW_DOMAINS; i++) {
		struct i915_fw_domain *d = &i915->fw[i];
		if (!(mask & d->mask))
			continue;
		if (d->count++ == 0) {
			if (i915_is_legacy(i915)) {
				i915_legacy_fw_wake(i915, d);
			} else {
				fw_domain_set(i915, d, 1);
				fw_domain_wait_ack(i915, d, 1);
			}
			i915->fw_held |= d->mask;
		}
	}
}

static void fw_domains_put_locked(struct i915_device *i915, uint32_t mask)
{
	for (int i = 0; i < I915_FW_DOMAINS; i++) {
		struct i915_fw_domain *d = &i915->fw[i];
		if (!(mask & d->mask))
			continue;
		if (d->count > 0 && --d->count == 0) {
			if (i915_is_legacy(i915))
				i915_legacy_fw_sleep(i915, d);
			else
				fw_domain_set(i915, d, 0);
			i915->fw_held &= ~d->mask;
		}
	}
}

void i915_fw_get(struct i915_device *i915, uint32_t mask)
{
	uint64_t fl;

	mask &= i915->fw_present;
	if (!mask)
		return;
	spin_lock_irqsave(&i915->fw_lock, &fl);
	fw_domains_get_locked(i915, mask);
	spin_unlock_irqrestore(&i915->fw_lock, fl);
}

void i915_fw_put(struct i915_device *i915, uint32_t mask)
{
	uint64_t fl;

	mask &= i915->fw_present;
	if (!mask)
		return;
	spin_lock_irqsave(&i915->fw_lock, &fl);
	fw_domains_put_locked(i915, mask);
	spin_unlock_irqrestore(&i915->fw_lock, fl);
}

/* ---- accessors -------------------------------------------------------------- */

/* An access the device did not claim -- a register that does not exist
 * on this part, or one in a unit that was asleep -- is recorded in the
 * debug register.  Checked after writes while bringing the driver up;
 * one line, then silence. */
static void check_unclaimed(struct i915_device *i915, uint32_t reg)
{
	if (i915_is_legacy(i915)) {
		i915_legacy_check_unclaimed(i915, reg);
		return;
	}
	if (i915->info->gen < 9 || i915->unclaimed_warned)
		return;
	uint32_t dbg = i915_read32_fw(i915, FPGA_DBG);
	if (dbg & FPGA_DBG_RM_NOCLAIM) {
		i915_write32_fw(i915, FPGA_DBG, FPGA_DBG_RM_NOCLAIM);
		i915->unclaimed_warned = 1;
		kprintf("[drm] i915: unclaimed register access (last %05x)\n",
			reg);
	}
}

uint32_t i915_read32(struct i915_device *i915, uint32_t reg)
{
	uint32_t fw = i915_fw_domains_for(i915, reg);
	int legacy = i915_is_legacy(i915);
	uint32_t v;
	uint64_t fl;

	if (!fw) {
		if (legacy)
			i915_legacy_mmio_read_prepare(i915, reg);
		return i915_read32_fw(i915, reg);
	}
	spin_lock_irqsave(&i915->fw_lock, &fl);
	fw_domains_get_locked(i915, fw);
	if (legacy)
		i915_legacy_mmio_read_prepare(i915, reg);
	v = i915_read32_fw(i915, reg);
	fw_domains_put_locked(i915, fw);
	spin_unlock_irqrestore(&i915->fw_lock, fl);
	return v;
}

void i915_write32(struct i915_device *i915, uint32_t reg, uint32_t val)
{
	int legacy = i915_is_legacy(i915);
	uint32_t fw;
	uint64_t fl;

	if (legacy)
		fw = i915_legacy_fw_domains_for(i915, reg, 1);
	else if (i915->info->gen_x10 >= 120)
		fw = gen12_fw_domains(i915, reg, 1);
	else
		fw = i915_fw_domains_for(i915, reg);

	if (!fw) {
		if (legacy)
			i915_legacy_mmio_write_prepare(i915, reg);
		i915_write32_fw(i915, reg, val);
		return;
	}
	spin_lock_irqsave(&i915->fw_lock, &fl);
	fw_domains_get_locked(i915, fw);
	if (legacy)
		i915_legacy_mmio_write_prepare(i915, reg);
	i915_write32_fw(i915, reg, val);
	check_unclaimed(i915, reg);
	fw_domains_put_locked(i915, fw);
	spin_unlock_irqrestore(&i915->fw_lock, fl);
}

uint64_t i915_read64(struct i915_device *i915, uint32_t reg)
{
	uint64_t lo = i915_read32(i915, reg);
	uint64_t hi = i915_read32(i915, reg + 4);
	return lo | (hi << 32);
}

void i915_write64(struct i915_device *i915, uint32_t reg, uint64_t val)
{
	i915_write32(i915, reg, (uint32_t)val);
	i915_write32(i915, reg + 4, (uint32_t)(val >> 32));
}

int i915_wait_reg(struct i915_device *i915, uint32_t reg, uint32_t mask,
		  uint32_t value, uint32_t timeout_us)
{
	for (uint32_t t = 0;; t += 10) {
		if ((i915_read32(i915, reg) & mask) == value)
			return 0;
		if (t >= timeout_us)
			return -ETIMEDOUT;
		lapic_delay_us(10);
	}
}

/* ---- setup -------------------------------------------------------------------- */

static void fw_domain_init(struct i915_device *i915, int idx, uint32_t mask,
			   uint32_t set, uint32_t ack)
{
	i915->fw[idx].mask = mask;
	i915->fw[idx].reg_set = set;
	i915->fw[idx].reg_ack = ack;
	i915->fw[idx].count = 0;
	i915->fw_present |= mask;
}

/* Xe2 on: which of the descriptor's video engines the media GT's fuses
 * leave, read with the media GT's own domain (index 9) awake.  The
 * register names the engines present; when the domain does not wake the
 * descriptor's list is kept. */
static uint32_t media_fused_engines(struct i915_device *i915, uint32_t engines)
{
	struct i915_fw_domain *d = &i915->fw[9];
	uint32_t fuse, vdbox, vebox;

	fw_domain_set(i915, d, 1);
	if (fw_domain_wait_ack(i915, d, 1)) {
		fw_domain_set(i915, d, 0);
		return engines;
	}
	fuse = i915_read32_fw(i915, I915_MEDIA_GT_BASE + GEN11_GT_VEBOX_VDBOX_DISABLE);
	fw_domain_set(i915, d, 0);
	vdbox = fuse & GEN11_GT_VDBOX_DISABLE_MASK;
	vebox = (fuse & GEN11_GT_VEBOX_DISABLE_MASK) >> GEN11_GT_VEBOX_DISABLE_SHIFT;
	for (unsigned n = 0; n < 4; n++)
		if (!(vdbox & (1u << n)))
			engines &= ~(I915_ENGINE_VCS0 << n);
	for (unsigned n = 0; n < 2; n++)
		if (!(vebox & (1u << n)))
			engines &= ~(I915_ENGINE_VECS0 << n);
	return engines;
}

int i915_uncore_init(struct i915_device *i915)
{
	const struct intel_device_info *info = i915->info;

	if (i915_is_legacy(i915))
		return i915_legacy_uncore_init(i915);
	spinlock_init(&i915->fw_lock, "i915_fw");
	i915->fw_present = 0;
	for (int i = 0; i < I915_FW_DOMAINS; i++)
		i915->fw[i].mask = 0;

	if (info->platform == I915_PLATFORM_CHERRYVIEW) {
		/* WaDisableShadowRegForCpd:chv */
		i915_write32_fw(i915, GTFIFOCTL,
				i915_read32_fw(i915, GTFIFOCTL) | GT_FIFO_CTL_BLOCK_ALL_POLICY_STALL |
					GT_FIFO_CTL_RC6_POLICY_STALL);
		fw_domain_init(i915, 0, I915_FW_RENDER, FORCEWAKE_VLV, FORCEWAKE_ACK_VLV);
		fw_domain_init(i915, 2, I915_FW_MEDIA, FORCEWAKE_MEDIA_VLV,
			       FORCEWAKE_ACK_MEDIA_VLV);
	} else if (info->gen == 8) {
		fw_domain_init(i915, 0, I915_FW_RENDER, FORCEWAKE_MT,
			       FORCEWAKE_ACK_HSW);
	} else if (info->gen <= 10) {
		fw_domain_init(i915, 0, I915_FW_RENDER, FORCEWAKE_RENDER_GEN9,
			       FORCEWAKE_ACK_RENDER_GEN9);
		fw_domain_init(i915, 1, I915_FW_GT, FORCEWAKE_GT_GEN9,
			       FORCEWAKE_ACK_GT_GEN9);
		fw_domain_init(i915, 2, I915_FW_MEDIA, FORCEWAKE_MEDIA_GEN9,
			       FORCEWAKE_ACK_MEDIA_GEN9);
	} else {
		uint32_t gt_ack = info->gen_x10 >= 127 ? FORCEWAKE_ACK_GT_MTL :
						       FORCEWAKE_ACK_GT_GEN9;
		fw_domain_init(i915, 0, I915_FW_RENDER, FORCEWAKE_RENDER_GEN9,
			       FORCEWAKE_ACK_RENDER_GEN9);
		fw_domain_init(i915, 1, I915_FW_GT, FORCEWAKE_GT_GEN12, gt_ack);
		/* Meteor Lake's media GT: its own GT domain, its video
		 * engines' and its security controller's, all at its
		 * offset. */
		if (i915->has_media_gt) {
			const uint32_t g = I915_MEDIA_GT_BASE;
			uint32_t engines = info->engine_mask;
			fw_domain_init(i915, 9, I915_FW_MEDIA_GT, g + FORCEWAKE_GT_GEN12,
				       g + FORCEWAKE_ACK_GT_MTL);
			/* Xe2 on: the video engines' domains are those of the
			 * engines the media fuses leave, which are read with
			 * the media GT awake */
			if (i915->gt_ip >= I915_IP(20, 0))
				engines = media_fused_engines(i915, engines);
			for (int n = 0; n < 4; n++)
				if (engines & (I915_ENGINE_VCS0 << n))
					fw_domain_init(i915, 3 + n, I915_FW_VDBOX0 << n,
						       g + FORCEWAKE_MEDIA_VDBOX_GEN11(n),
						       g + FORCEWAKE_ACK_MEDIA_VDBOX_GEN11(n));
			for (int n = 0; n < 2; n++)
				if (engines & (I915_ENGINE_VECS0 << n))
					fw_domain_init(i915, 7 + n, I915_FW_VEBOX0 << n,
						       g + FORCEWAKE_MEDIA_VEBOX_GEN11(n),
						       g + FORCEWAKE_ACK_MEDIA_VEBOX_GEN11(n));
			/* the security controller's domain: on Xe2 only where
			 * the part has a GSC (Battlemage has none) */
			if (i915->gt_ip < I915_IP(20, 0) || info->gsc_fw)
				fw_domain_init(i915, 10, I915_FW_GSC, g + FORCEWAKE_REQ_GSC,
					       g + FORCEWAKE_ACK_GSC);
		}
		/* Meteor Lake's video engines are on a separate media GT
		 * (register base 0x380000) with forcewake of its own; the
		 * primary GT has no media domains, and requests written to
		 * these offsets are never acknowledged. */
		for (int n = 0; n < 4 && info->gen_x10 < 127; n++) {
			if (!(info->engine_mask & (I915_ENGINE_VCS0 << n)))
				continue;
			fw_domain_init(i915, 3 + n, I915_FW_VDBOX0 << n,
				       FORCEWAKE_MEDIA_VDBOX_GEN11(n),
				       FORCEWAKE_ACK_MEDIA_VDBOX_GEN11(n));
		}
		for (int n = 0; n < 2 && info->gen_x10 < 127; n++) {
			if (!(info->engine_mask & (I915_ENGINE_VECS0 << n)))
				continue;
			fw_domain_init(i915, 7 + n, I915_FW_VEBOX0 << n,
				       FORCEWAKE_MEDIA_VEBOX_GEN11(n),
				       FORCEWAKE_ACK_MEDIA_VEBOX_GEN11(n));
		}
	}

	/* Start from a known state: every domain released, then the
	 * firmware's unclaimed-access flag cleared. */
	for (int i = 0; i < I915_FW_DOMAINS; i++) {
		if (!i915->fw[i].mask)
			continue;
		i915_write32_fw(i915, i915->fw[i].reg_set, 0xffff0000u);
		(void)i915_read32_fw(i915, i915->fw[i].reg_ack);
	}
	if (info->gen >= 9)
		i915_write32_fw(i915, FPGA_DBG, FPGA_DBG_RM_NOCLAIM);

	/* Prove the mechanism on the render domain: wake it, expect the
	 * acknowledge, release it. */
	{
		uint64_t fl;
		int rc;
		spin_lock_irqsave(&i915->fw_lock, &fl);
		i915->fw[0].count++;
		fw_domain_set(i915, &i915->fw[0], 1);
		rc = fw_domain_wait_ack(i915, &i915->fw[0], 1);
		i915->fw[0].count--;
		fw_domain_set(i915, &i915->fw[0], 0);
		spin_unlock_irqrestore(&i915->fw_lock, fl);
		if (rc)
			return rc;
	}
	return 0;
}

void i915_uncore_fini(struct i915_device *i915)
{
	if (i915_is_legacy(i915)) {
		i915_legacy_uncore_fini(i915);
		return;
	}
	for (int i = 0; i < I915_FW_DOMAINS; i++)
		if (i915->fw[i].mask)
			i915_write32_fw(i915, i915->fw[i].reg_set, 0xffff0000u);
	i915->fw_present = 0;
}
