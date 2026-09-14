// LikeOS-64 -- Intel integrated graphics: probe and the DRM backend.
//
// The device is found on the PCI bus, named from the table, and brought
// up in layers: registers and forcewake, the global GTT and stolen
// memory, the interrupt vector, then the DRM device node.  Every layer
// can refuse, and a refusal at any point tears down what came before and
// leaves the machine on the boot framebuffer -- the driver's presence
// must never cost a screen.
#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/uapi/drm/i915_drm.h>
#include <kernel/uapi/ioctl.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/uaccess.h>
#include <kernel/mm/memory.h>

struct i915_device g_i915;

/* ---- PCH detection ------------------------------------------------------- */

struct pch_id {
	uint16_t base, mask;
	uint8_t pch;
	const char *name;
};

static const struct pch_id pch_ids[] = {
	{ 0x8c00, 0xff00, I915_PCH_LPT, "Lynx Point" },
	{ 0x9c00, 0xff00, I915_PCH_LPT, "Lynx Point LP" },
	{ 0x8c80, 0xff80, I915_PCH_WPT, "Wildcat Point" },
	{ 0x9c80, 0xff80, I915_PCH_WPT, "Wildcat Point LP" },
	{ 0xa100, 0xff00, I915_PCH_SPT, "Sunrise Point" },
	{ 0x9d00, 0xff00, I915_PCH_SPT, "Sunrise Point LP" },
	{ 0xa280, 0xff80, I915_PCH_KBP, "Kaby Point" },
	{ 0xa300, 0xff00, I915_PCH_CNP, "Cannon Point" },
	{ 0x9d80, 0xff80, I915_PCH_CNP, "Cannon Point LP" },
	{ 0x0280, 0xff80, I915_PCH_CMP, "Comet Point LP" },
	{ 0x0680, 0xff80, I915_PCH_CMP, "Comet Point" },
	{ 0x4b00, 0xff80, I915_PCH_CMP, "Comet Point V" },
	{ 0x3480, 0xff80, I915_PCH_ICP, "Ice Point" },
	{ 0x3880, 0xff80, I915_PCH_ICP, "Mule Creek Canyon" },
	{ 0x4d80, 0xff80, I915_PCH_JSP, "Jasper Point" },
	{ 0xa080, 0xff80, I915_PCH_TGP, "Tiger Point" },
	{ 0x4380, 0xff80, I915_PCH_TGP, "Tiger Point" },
	{ 0x7a80, 0xff80, I915_PCH_ADP, "Alder Point" },
	{ 0x5180, 0xff80, I915_PCH_ADP, "Alder Point-P" },
	{ 0x7a00, 0xff80, I915_PCH_ADP, "Alder Point-S" },
	{ 0x5480, 0xff80, I915_PCH_ADP, "Alder Point-N" },
	{ 0x7e00, 0xff80, I915_PCH_MTP, "Meteor Point" },
	{ 0xae00, 0xff80, I915_PCH_MTP, "Meteor Point" },
	{ 0xa800, 0xff80, I915_PCH_LNL, "Lunar Lake" },
};

static const char *i915_pch_detect(struct i915_device *i915)
{
	const pci_device_t *isa = pci_find_bdf(0, 0x1f, 0);

	i915->pch = I915_PCH_NONE;
	i915->pch_devid = 0;
	if (!(i915->info->flags & I915_INFO_HAS_PCH))
		return "none";
	if (!isa || isa->vendor_id != 0x8086) {
		i915->pch = I915_PCH_UNKNOWN;
		return "unknown (no ISA bridge)";
	}
	i915->pch_devid = isa->device_id;
	for (unsigned i = 0; i < sizeof(pch_ids) / sizeof(pch_ids[0]); i++) {
		if ((isa->device_id & pch_ids[i].mask) == pch_ids[i].base) {
			i915->pch = pch_ids[i].pch;
			return pch_ids[i].name;
		}
	}
	i915->pch = I915_PCH_UNKNOWN;
	return "unknown";
}

/* ---- the DRM backend (grows with the driver) ----------------------------- */

static int i915_gem_init_obj(struct drm_gem_object *o)
{
	struct i915_bo *bo = kalloc(sizeof(*bo));
	if (!bo)
		return -ENOMEM;
	mm_memset(bo, 0, sizeof(*bo));
	o->priv = bo;
	return 0;
}

static void i915_gem_free_obj(struct drm_gem_object *o)
{
	i915_gem_object_free(o);
}

/* User mappings of buffers: write-combining by default, so a display
 * server's writes stream to memory the display engine reads without a
 * flush; the plain cache kinds when a client asks for them by name. */
static uint64_t i915_gem_mmap_pte(struct drm_gem_object *o, unsigned kind)
{
	(void)o;
	switch (kind) {
	case 2: /* write-back */
		return 0;
	case 3: /* uncached */
		return PAGE_WRITE_THROUGH | PAGE_CACHE_DISABLE;
	default: /* write-combining */
		return PAGE_WRITE_THROUGH;
	}
}

static uint64_t i915_gem_page_phys(struct drm_gem_object *o, uint64_t index)
{
	if (!o->pages || index >= o->npages)
		return (uint64_t)-1;
	return o->pages[index];
}

static int i915_render_allowed(unsigned nr)
{
	(void)nr;
	return 1;
}

/* The driver's ioctls, routed by number to their files. */
static long i915_ioctl(struct drm_device *dev, struct drm_file *fp,
		       unsigned nr, unsigned dir, void *kbuf, unsigned size,
		       int *handled)
{
	struct i915_device *i915 = to_i915(dev);
	(void)dir;

	*handled = 1;
	switch (nr) {
	case DRM_I915_GETPARAM:
		if (size < sizeof(struct drm_i915_getparam))
			return -EINVAL;
		return i915_getparam(i915, kbuf);
	case DRM_I915_SETPARAM:
		return -EINVAL;
	case DRM_I915_GEM_EXECBUFFER2:
		if (size < sizeof(struct drm_i915_gem_execbuffer2))
			return -EINVAL;
		return i915_gem_execbuffer2(i915, fp, kbuf, dir & _IOC_READ);
	case DRM_I915_QUERY:
		if (size < sizeof(struct drm_i915_query))
			return -EINVAL;
		return i915_query_ioctl(i915, fp, kbuf);
	case DRM_I915_GEM_CONTEXT_CREATE:
	case DRM_I915_GEM_CONTEXT_DESTROY:
	case DRM_I915_GEM_CONTEXT_GETPARAM:
	case DRM_I915_GEM_CONTEXT_SETPARAM:
	case DRM_I915_GEM_VM_CREATE:
	case DRM_I915_GEM_VM_DESTROY:
	case DRM_I915_GET_RESET_STATS:
	case DRM_I915_REG_READ:
		return i915_context_ioctl(i915, fp, nr, kbuf, size, handled);
	case DRM_I915_PERF_OPEN:
	case DRM_I915_PERF_ADD_CONFIG:
	case DRM_I915_PERF_REMOVE_CONFIG:
	case DRM_I915_GET_PIPE_FROM_CRTC_ID:
	case DRM_I915_OVERLAY_PUT_IMAGE:
	case DRM_I915_OVERLAY_ATTRS:
	case DRM_I915_SET_SPRITE_COLORKEY:
	case DRM_I915_GET_SPRITE_COLORKEY:
		return -ENODEV;
	default:
		return i915_gem_ioctl(i915, fp, nr, kbuf, size, handled);
	}
}

/* Once the root filesystem is there: the GT's worker thread (and, later,
 * firmware). */
/* A submission that cannot get memory is reported once and then in
 * silence: the userspace driver retries such a failure in a tight loop,
 * so the interesting thing is WHICH allocation failed, said once, with
 * what the machine had free at that moment. */
/* A binding that fails says where it was asked to put the object: the
 * address is the client's choice, and an address the space cannot hold
 * looks exactly like no memory from the call's return value. */
void i915_bind_failed_once(struct i915_device *i915, uint64_t addr, uint64_t size,
			   int pinned)
{
	if (i915->enomem_warned)
		return;
	i915->enomem_warned = 1;
	kprintf("[drm] i915: cannot bind %llu KB at %llx (%s, %llu MB of free pages)\n",
		(unsigned long long)(size / 1024), (unsigned long long)addr,
		pinned ? "the client chose the address" : "the driver chose it",
		(unsigned long long)(mm_get_free_pages() * 4096 / (1024 * 1024)));
}

void i915_enomem_once(struct i915_device *i915, const char *what)
{
	if (i915->enomem_warned)
		return;
	i915->enomem_warned = 1;
	kprintf("[drm] i915: out of memory: %s (%llu MB of free pages)\n", what,
		(unsigned long long)(mm_get_free_pages() * 4096 / (1024 * 1024)));
}

/* ---- power ------------------------------------------------------------------- */

static int i915_suspend(struct drm_device *dev)
{
	struct i915_device *i915 = to_i915(dev);

	/* Submissions stop first; whatever is in flight is thrown away by
	 * the reset, its fences signalled with an error by the engine code. */
	if (i915->gt_ready) {
		i915->gt_ready = 0;
		for (int i = 0; i < I915_NUM_ENGINES; i++) {
			struct i915_engine *e = &i915->engines[i];
			if (e->present)
				(void)i915_engine_idle(e, 200);
		}
		i915_gt_reset_all(i915);
	}
	intel_display_suspend(i915);
	i915_irq_suspend(i915);
	i915_uncore_fini(i915);
	return 0;
}

static int i915_resume(struct drm_device *dev)
{
	struct i915_device *i915 = to_i915(dev);
	int rc;

	rc = i915_uncore_init(i915);
	if (rc) {
		kprintf("[drm] i915: forcewake did not answer after resume\n");
		return rc;
	}
	i915_ggtt_program_pat(i915);
	i915_ggtt_rewrite_all(i915);
	i915_irq_resume(i915);
	rc = intel_display_resume(i915);
	if (rc)
		return rc;
	if (i915->nengines) {
		i915_engines_resume(i915);
		i915->gt_ready = 1;
	}
	return 0;
}

static int i915_late_init(struct drm_device *dev)
{
	struct i915_device *i915 = to_i915(dev);
	if (i915->display.ready)
		(void)intel_dmc_load(i915);
	if (i915_guc_wants_load(i915)) {
		/* Where the GuC is the only way to submit, the engines come
		 * up after it and run their first batch through it. */
		if (i915_guc_init(i915) == 0 && (i915->info->flags & I915_INFO_GUC_MANDATORY) &&
		    i915_engines_init(i915) == 0) {
			i915->gt_ready = 1;
			if (i915_guc_submission_enable(i915) != 0 || i915_gt_golden_init(i915) != 0) {
				kprintf("[drm] i915: no engine executed its first batch through the GuC; rendering disabled\n");
				i915->gt_ready = 0;
				i915_engines_fini(i915);
			}
		}
	} else {
		i915_uc_firmware_describe(i915);
	}
	(void)intel_hpd_start(i915);
	return i915_gt_workers_start(i915);
}

static const struct drm_driver i915_driver = {
	.name = "i915",
	.desc = "Intel Graphics",
	.date = "20260912",
	.major = 1,
	.minor = 6,
	.patch = 0,
	.cursor_w = 256,
	.cursor_h = 256,
	.open = i915_gem_open,
	.postclose = i915_gem_postclose,
	.late_init = i915_late_init,
	.gem_init = i915_gem_init_obj,
	.gem_free = i915_gem_free_obj,
	.gem_release_pages = i915_gem_release_pages,
	.gem_page_phys = i915_gem_page_phys,
	.gem_mmap_pte_extra = PAGE_WRITE_THROUGH,
	.gem_mmap_pte = i915_gem_mmap_pte,
	.atomic_check = intel_atomic_check,
	.atomic_commit = intel_atomic_commit,
	.gamma_size = 256,
	.suspend = i915_suspend,
	.resume = i915_resume,
	.fb_dirty = intel_fb_dirty,
	.fb_formats = intel_fb_formats,
	.nfb_formats = 7,
	.fb_modifiers = intel_fb_modifiers,
	.nfb_modifiers = 3,
	.fb_check = intel_fb_check,
	.detect = intel_detect,
	.get_modes = intel_get_modes,
	.hw_vblank = 1,
	.display_verify = intel_display_verify,
	.display_fallback = intel_display_fallback,
	.ioctl = i915_ioctl,
	.render_allowed = i915_render_allowed,
};

/* ---- probe ---------------------------------------------------------------- */

static const pci_device_t *i915_find_device(void)
{
	int count = 0;
	const pci_device_t *devs = pci_get_devices(&count);
	const pci_device_t *found = NULL;

	for (int i = 0; i < count; i++) {
		const pci_device_t *d = &devs[i];
		if (d->vendor_id != 0x8086 || d->class_code != 0x03)
			continue;
		/* The integrated device is function 0 of device 2 on bus
		 * 0; take it over any other Intel display device (a
		 * discrete card is not driven by this driver's display
		 * path in a hybrid machine). */
		if (d->bus == 0 && d->device == 2 && d->function == 0)
			return d;
		if (!found)
			found = d;
	}
	return found;
}

static void i915_teardown(struct i915_device *i915)
{
	if (i915->gt_ready) {
		i915->gt_ready = 0;
		i915_engines_fini(i915);
	}
	if (i915->display.ready)
		intel_display_fini(i915);
	i915_irq_fini(i915);
	i915_gtt_fini(i915);
	i915_uncore_fini(i915);
	if (i915->mmio_virt) {
		mm_unmap_mmio(i915->mmio_virt, i915->mmio_size / 4096);
		i915->mmio_virt = 0;
	}
}

int i915_init(void)
{
	struct i915_device *i915 = &g_i915;
	const pci_device_t *pci = i915_find_device();
	const struct i915_pci_id *id;
	int rc;

	if (!pci)
		return -ENODEV;
	id = i915_pci_lookup(pci->device_id);
	if (!id) {
		kprintf("[drm] i915: unknown Intel display device %04x; keeping the boot framebuffer\n",
			pci->device_id);
		return -ENODEV;
	}
	mm_memset(i915, 0, sizeof(*i915));
	i915->pci = pci;
	i915->id = id;
	i915->info = id->info;
	i915->devid = pci->device_id;
	i915->revid = pci_cfg_read8(pci, 0x08);
	i915->irq_vector = -1;

	if (i915->info->flags & I915_INFO_NAME_ONLY) {
		kprintf("[drm] i915: %s (%s, generation %u, %04x) is not driven; keeping the boot framebuffer\n",
			id->name, i915->info->name, i915->info->gen,
			pci->device_id);
		return -ENODEV;
	}
	kprintf("[drm] i915: %s (%s GT%u, generation %u.%u, %04x rev %02x)\n",
		id->name, i915->info->name, id->gt ? id->gt : i915->info->gt,
		i915->info->gen_x10 / 10, i915->info->gen_x10 % 10,
		pci->device_id, i915->revid);

	/* The register window and the aperture. */
	if (pci_bar_decode(pci, 0, &i915->bar_mmio) != 0 ||
	    (i915->bar_mmio.flags & PCI_BAR_IO)) {
		kprintf("[drm] i915: no register window (BAR0)\n");
		return -ENODEV;
	}
	if (pci_bar_decode(pci, 2, &i915->bar_aperture) != 0)
		i915->bar_aperture.size = 0;
	if (pci_bar_decode(pci, 4, &i915->bar_io) != 0)
		i915->bar_io.size = 0;
	i915_dbg("[drm] i915: registers at %llx (%llu MB), aperture at %llx (%llu MB)\n",
		(unsigned long long)i915->bar_mmio.base,
		(unsigned long long)(i915->bar_mmio.size >> 20),
		(unsigned long long)i915->bar_aperture.base,
		(unsigned long long)(i915->bar_aperture.size >> 20));

	pci_enable_busmaster_mem(pci);

	/* Registers are the lower half of BAR0 on every part driven here;
	 * the upper half is the GTT (i915_gtt.c). */
	i915->mmio_size = i915->bar_mmio.size / 2;
	if (i915->mmio_size < (2u << 20)) {
		kprintf("[drm] i915: register window too small\n");
		return -ENODEV;
	}
	i915->mmio_virt = mm_map_mmio_flags(i915->bar_mmio.base,
					    i915->mmio_size / 4096, MM_MMIO_UC);
	if (!i915->mmio_virt) {
		kprintf("[drm] i915: cannot map the registers\n");
		return -ENOMEM;
	}

	const char *pch = i915_pch_detect(i915);
	i915_dbg("[drm] i915: PCH %s (%04x)\n", pch, i915->pch_devid);

	rc = i915_uncore_init(i915);
	if (rc) {
		kprintf("[drm] i915: forcewake did not answer; keeping the boot framebuffer\n");
		i915_teardown(i915);
		return rc;
	}
	i915_read_topology(i915);
	i915->cs_timestamp_hz = i915_timestamp_hz(i915);
	i915_dbg("[drm] i915: slices %x, subslices %x, %u EUs, timestamp %u Hz\n",
		i915->slice_mask, i915->subslice_mask[0], i915->eu_total,
		i915->cs_timestamp_hz);

	rc = i915_gtt_probe(i915);
	if (rc) {
		i915_teardown(i915);
		return rc;
	}
	(void)i915_irq_init(i915); /* without one the driver still works */

	rc = drm_dev_register(&i915->drm, &i915_driver, pci, i915);
	if (rc) {
		kprintf("[drm] i915: device registration failed (%d)\n", rc);
		i915_teardown(i915);
		return rc;
	}

	/* The display: outputs, the panel, its modes.  Failure here leaves
	 * the node registered (the render side does not need a screen) and
	 * the console on the boot framebuffer. */
	rc = intel_display_init(i915);
	if (rc) {
		kprintf("[drm] i915: display not brought up (%d); console stays on the boot framebuffer\n",
			rc);
		return 0;
	}
	/* The console onto the panel's own mode.  The core refuses when the
	 * first connector has no modes; a refusal or a verify failure puts
	 * the boot framebuffer back (drm_console.c). */
	if (i915->drm.nconn && i915->drm.conn[0].nmodes)
		(void)drm_console_takeover(&i915->drm);
	else
		kprintf("[drm] i915: no modes on the first connector; console stays on the boot framebuffer\n");

	/* The GT: engines up, one batch each.  Failure leaves the device a
	 * display-only one (the render node answers ENODEV). */
	{
		memory_stats_t st;
		mm_get_memory_stats(&st);
		i915->total_ram_bytes = st.total_memory;
	}
	if (i915->info->flags & I915_INFO_GUC_MANDATORY) {
		/* the GT waits for the firmware, which needs the root
		 * filesystem: the late-init hook brings it up */
		kprintf("[drm] i915: %s submits through the GuC; the GT comes up after the firmware loads\n",
			i915->info->name);
	} else if (i915->info->gen_x10 >= 120) {
		kprintf("[drm] i915: execution lists on %s are not brought up yet; display only\n",
			i915->info->name);
	} else if (i915_engines_init(i915) == 0) {
		i915->gt_ready = 1;
		if (i915_gt_golden_init(i915) != 0) {
			kprintf("[drm] i915: no engine executed its first batch; rendering disabled\n");
			i915->gt_ready = 0;
			i915_engines_fini(i915);
		}
	}
	return 0;
}
