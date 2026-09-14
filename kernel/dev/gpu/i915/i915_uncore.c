// LikeOS-64 -- register access and forcewake for Intel graphics.
//
// The GT's register file is not always powered: the hardware puts render,
// media and GT-common units to sleep on their own, and a register read
// from a sleeping unit returns nonsense (or hangs on some steppings).
// Forcewake is the request to keep a unit awake -- write the request bit,
// wait for the acknowledge -- and every GT register access below takes
// the domains its offset belongs to, refcounted so nested users pay once.
// Display registers need none.
#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

#define FW_ACK_TIMEOUT_US 50000

static inline volatile uint32_t *reg32(struct i915_device *i915, uint32_t reg)
{
	return (volatile uint32_t *)(i915->mmio_virt + reg);
}

uint32_t i915_read32_fw(struct i915_device *i915, uint32_t reg)
{
	return *reg32(i915, reg);
}

void i915_write32_fw(struct i915_device *i915, uint32_t reg, uint32_t val)
{
	*reg32(i915, reg) = val;
}

/* ---- which domains a register lives in ------------------------------------ */

/* Ranges are the GT's own map; anything display-side (0x40000 and up on
 * the north side, the PCH from 0xc0000) needs no forcewake.  Unknown GT
 * ranges take every domain, which is slower but never wrong. */
uint32_t i915_fw_domains_for(struct i915_device *i915, uint32_t reg)
{
	const struct intel_device_info *info = i915->info;

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
	if (info->gen < 11 && reg >= 0x1a000 && reg < 0x1c000)
		return I915_FW_MEDIA; /* VECS0 */
	if (reg >= 0x1c000 && reg < 0x22000)
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
			fw_domain_set(i915, d, 1);
			fw_domain_wait_ack(i915, d, 1);
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
	uint32_t v;
	uint64_t fl;

	if (!fw)
		return i915_read32_fw(i915, reg);
	spin_lock_irqsave(&i915->fw_lock, &fl);
	fw_domains_get_locked(i915, fw);
	v = i915_read32_fw(i915, reg);
	fw_domains_put_locked(i915, fw);
	spin_unlock_irqrestore(&i915->fw_lock, fl);
	return v;
}

void i915_write32(struct i915_device *i915, uint32_t reg, uint32_t val)
{
	uint32_t fw = i915_fw_domains_for(i915, reg);
	uint64_t fl;

	if (!fw) {
		i915_write32_fw(i915, reg, val);
		return;
	}
	spin_lock_irqsave(&i915->fw_lock, &fl);
	fw_domains_get_locked(i915, fw);
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

int i915_uncore_init(struct i915_device *i915)
{
	const struct intel_device_info *info = i915->info;

	spinlock_init(&i915->fw_lock, "i915_fw");
	i915->fw_present = 0;
	for (int i = 0; i < I915_FW_DOMAINS; i++)
		i915->fw[i].mask = 0;

	if (info->gen == 8) {
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
		for (int n = 0; n < 4; n++) {
			if (!(info->engine_mask & (I915_ENGINE_VCS0 << n)))
				continue;
			fw_domain_init(i915, 3 + n, I915_FW_VDBOX0 << n,
				       FORCEWAKE_MEDIA_VDBOX_GEN11(n),
				       FORCEWAKE_ACK_MEDIA_VDBOX_GEN11(n));
		}
		for (int n = 0; n < 2; n++) {
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
	for (int i = 0; i < I915_FW_DOMAINS; i++)
		if (i915->fw[i].mask)
			i915_write32_fw(i915, i915->fw[i].reg_set, 0xffff0000u);
	i915->fw_present = 0;
}
