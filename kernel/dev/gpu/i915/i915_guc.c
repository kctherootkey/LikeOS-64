// LikeOS-64 -- the GuC: firmware load, the command transport, submission.
//
// The GuC is a microcontroller in the GT.  Loaded with its firmware it
// can own command submission (the host registers contexts and asks
// for them to be scheduled; the GuC writes the execution lists) and
// authenticate the HuC, the media controller.  Where a part's design
// makes GuC submission the only kind (Gen12.5 and later) this file is
// the whole path; on Ice Lake and Tiger Lake the firmware is loaded
// the way the platform is meant to run, with the HuC authenticated
// and submission left with the execution lists; Gen9 keeps the
// execution lists alone.
//
// Nothing here has run on hardware: the ThinkPad P50 (Skylake) this
// driver was written on takes no GuC by this policy.  The register
// and message formats are those of firmware major version 70, which is
// the version named in the device table.
#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/i915_gt.h>
#include <kernel/dev/gpu/i915/i915_lrc_layout.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/firmware.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/uapi/drm/i915_drm.h>

/* ---- the firmware wrapper -------------------------------------------------------- */

#define UC_CSS_HEADER_BYTES 128
#define UC_CSS_MODULE_TYPE 6

struct uc_image {
	void *data; /* the file (firmware_request's) */
	size_t len;
	uint32_t ucode_size, rsa_size, version;
};

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static int uc_image_open(const char *what, const char *name, struct uc_image *img)
{
	int rc = firmware_request(name, &img->data, &img->len);
	if (rc) {
		kprintf("[drm] i915: %s firmware %s not available (%d)\n", what, name, rc);
		return rc;
	}
	const uint8_t *css = img->data;
	if (img->len < UC_CSS_HEADER_BYTES || rd32(css + 0) != UC_CSS_MODULE_TYPE ||
	    rd32(css + 4) * 4 != UC_CSS_HEADER_BYTES) {
		kprintf("[drm] i915: %s firmware %s: not a firmware image\n", what, name);
		firmware_release(img->data);
		return -EINVAL;
	}
	uint32_t size_dw = rd32(css + 6 * 4);
	uint32_t key_dw = rd32(css + 7 * 4);
	if (size_dw < UC_CSS_HEADER_BYTES / 4 || key_dw != UOS_RSA_SCRATCH_COUNT) {
		kprintf("[drm] i915: %s firmware %s: unexpected layout (%u dwords, key %u)\n",
			what, name, size_dw, key_dw);
		firmware_release(img->data);
		return -EINVAL;
	}
	img->ucode_size = (size_dw - UC_CSS_HEADER_BYTES / 4) * 4;
	img->rsa_size = key_dw * 4;
	img->version = rd32(css + 16 * 4);
	if (img->len < UC_CSS_HEADER_BYTES + img->ucode_size + img->rsa_size) {
		kprintf("[drm] i915: %s firmware %s: truncated\n", what, name);
		firmware_release(img->data);
		return -EINVAL;
	}
	return 0;
}

/* The image into a GGTT object the DMA engine reads from. */
static struct drm_gem_object *image_to_ggtt(struct i915_device *i915, const struct uc_image *img,
					    uint32_t *ggtt)
{
	uint64_t size = (img->len + 4095) & ~4095ULL;
	struct drm_gem_object *o = i915_gem_alloc_bound(i915, size, 0, 1, ggtt);
	if (!o)
		return NULL;
	mm_memcpy(phys_to_virt(o->pages[0]), img->data, img->len);
	__asm__ volatile("mfence" ::: "memory");
	return o;
}

/* ---- register access helpers ----------------------------------------------------- */

static int wait_reg(struct i915_device *i915, uint32_t reg, uint32_t mask, uint32_t want, int ms)
{
	for (int t = 0; t < ms * 100; t++) {
		if ((i915_read32(i915, reg) & mask) == want)
			return 0;
		lapic_delay_us(10);
	}
	return -ETIMEDOUT;
}

void i915_guc_ggtt_invalidate(struct i915_device *i915)
{
	if (!i915->guc.loaded && !i915->guc.wanted)
		return;
	if (i915->info->gen >= 12)
		i915_write32(i915, GEN12_GUC_TLB_INV_CR, GEN12_GUC_TLB_INV_CR_INVALIDATE);
	else
		i915_write32(i915, GEN8_GTCR, GEN8_GTCR_INVALIDATE);
}

/* ---- the messages ------------------------------------------------------------------ */

/* Every message starts with a "HXG" dword: who sent it, what kind it
 * is, and for a request the action. */
#define HXG_ORIGIN_GUC (1u << 31)
#define HXG_TYPE_SHIFT 28
#define HXG_TYPE_MASK (7u << 28)
#define HXG_TYPE_REQUEST 0u
#define HXG_TYPE_FAST_REQUEST 2u
#define HXG_TYPE_NO_RESPONSE_BUSY 3u
#define HXG_TYPE_NO_RESPONSE_RETRY 5u
#define HXG_TYPE_RESPONSE_FAILURE 6u
#define HXG_TYPE_RESPONSE_SUCCESS 0u
#define HXG_TYPE_EVENT 7u
#define HXG_REQUEST_DATA0(x) ((uint32_t)(x) << 16)
#define HXG_ACTION_MASK 0xffffu
#define HXG_RESPONSE_DATA0(x) ((x) & 0x0fffffffu)
#define HXG_FAILURE_ERROR(x) (((x) >> 16) & 0xfffu)
#define HXG_FAILURE_HINT(x) ((x) & 0xffffu)

#define GUC_ACTION_HOST2GUC_SELF_CFG 0x0508
#define GUC_ACTION_HOST2GUC_CONTROL_CTB 0x0509
#define GUC_ACTION_AUTHENTICATE_HUC 0x4000
#define GUC_ACTION_REGISTER_CONTEXT 0x4502
#define GUC_ACTION_DEREGISTER_CONTEXT 0x4503
#define GUC_ACTION_SCHED_CONTEXT 0x1000
#define GUC_ACTION_SCHED_CONTEXT_MODE_SET 0x1001
#define GUC_ACTION_UPDATE_CONTEXT_POLICIES 0x503f
#define GUC2HOST_SCHED_CONTEXT_MODE_DONE 0x1002
#define GUC2HOST_CONTEXT_RESET_NOTIFICATION 0x1008
#define GUC2HOST_ENGINE_FAILURE_NOTIFICATION 0x1009
#define GUC2HOST_DEREGISTER_CONTEXT_DONE 0x4504
#define GUC2HOST_NOTIFY_FLUSH_LOG 0x4003
#define GUC2HOST_NOTIFY_CRASH_DUMP 0x4005
#define GUC2HOST_NOTIFY_EXCEPTION 0x4006

#define KLV(key, len) (((uint32_t)(key) << 16) | (uint32_t)(len))
#define KLV_SELF_CFG_H2G_CTB_ADDR 0x0902
#define KLV_SELF_CFG_H2G_CTB_DESC_ADDR 0x0903
#define KLV_SELF_CFG_H2G_CTB_SIZE 0x0904
#define KLV_SELF_CFG_G2H_CTB_ADDR 0x0905
#define KLV_SELF_CFG_G2H_CTB_DESC_ADDR 0x0906
#define KLV_SELF_CFG_G2H_CTB_SIZE 0x0907
#define KLV_CTX_EXECUTION_QUANTUM 0x2001
#define KLV_CTX_PREEMPTION_TIMEOUT 0x2002
#define KLV_CTX_SCHEDULING_PRIORITY 0x2003
#define GUC_CLIENT_PRIORITY_KMD_NORMAL 2
#define GUC_CTB_CONTROL_ENABLE 1
#define GUC_CONTEXT_ENABLE 1
#define GUC_CONTEXT_DISABLE 0
#define CONTEXT_REGISTRATION_FLAG_KMD (1u << 0)

/* The registers a request goes through when the transport is not up
 * yet (and the HuC's authentication, which needs an answer). */
static uint32_t send_reg(struct i915_device *i915, int n)
{
	return i915->info->gen >= 11 ? GEN11_SOFT_SCRATCH(n) : SOFT_SCRATCH(n);
}

static void guc_notify(struct i915_device *i915)
{
	if (i915->info->gen >= 11)
		i915_write32(i915, GEN11_GUC_HOST_INTERRUPT, GUC_SEND_TRIGGER);
	else
		i915_write32(i915, GUC_SEND_INTERRUPT, GUC_SEND_TRIGGER);
}

/* A request over the scratch registers: up to four dwords out, the
 * reply's dwords back (response[0] is the header). */
static int guc_send_mmio(struct i915_device *i915, uint32_t action, const uint32_t *data,
			 int ndata, uint32_t *response, int nresp)
{
	int n = 1 + ndata;
	if (n > 4)
		return -EINVAL;
	i915_write32(i915, send_reg(i915, 0), HXG_REQUEST_DATA0(0) | (HXG_TYPE_REQUEST << HXG_TYPE_SHIFT) | action);
	for (int i = 0; i < ndata; i++)
		i915_write32(i915, send_reg(i915, 1 + i), data[i]);
	for (int i = n; i < 4; i++)
		i915_write32(i915, send_reg(i915, i), 0);
	(void)i915_read32(i915, send_reg(i915, 0));
	guc_notify(i915);
	/* the GuC answers in the same registers: busy first, then the reply */
	uint32_t hdr = 0;
	for (int t = 0; t < 100000; t++) { /* up to a second */
		hdr = i915_read32(i915, send_reg(i915, 0));
		uint32_t type = (hdr & HXG_TYPE_MASK) >> HXG_TYPE_SHIFT;
		if ((hdr & HXG_ORIGIN_GUC) && type != HXG_TYPE_NO_RESPONSE_BUSY &&
		    type != HXG_TYPE_NO_RESPONSE_RETRY)
			break;
		if ((hdr & HXG_ORIGIN_GUC) && type == HXG_TYPE_NO_RESPONSE_RETRY && t > 1000) {
			/* asked to try again: resend once */
			i915_write32(i915, send_reg(i915, 0),
				     (HXG_TYPE_REQUEST << HXG_TYPE_SHIFT) | action);
			guc_notify(i915);
		}
		lapic_delay_us(10);
	}
	uint32_t type = (hdr & HXG_TYPE_MASK) >> HXG_TYPE_SHIFT;
	if (!(hdr & HXG_ORIGIN_GUC) || type == HXG_TYPE_NO_RESPONSE_BUSY ||
	    type == HXG_TYPE_NO_RESPONSE_RETRY) {
		kprintf("[drm] i915: GuC did not answer action %04x (header %08x)\n", action, hdr);
		return -ETIMEDOUT;
	}
	if (type == HXG_TYPE_RESPONSE_FAILURE) {
		kprintf("[drm] i915: GuC refused action %04x: error %x, hint %x\n", action,
			HXG_FAILURE_ERROR(hdr), HXG_FAILURE_HINT(hdr));
		return -EIO;
	}
	if (type != HXG_TYPE_RESPONSE_SUCCESS) {
		kprintf("[drm] i915: GuC answered action %04x with message type %u\n", action, type);
		return -EIO;
	}
	for (int i = 0; i < nresp; i++)
		response[i] = i915_read32(i915, send_reg(i915, i));
	return 0;
}

/* ---- the additional data structures ----------------------------------------------- */

#define GUC_MAX_ENGINE_CLASSES 16
#define GUC_MAX_INSTANCES_PER_CLASS 32
#define GUC_CAPTURE_LIST_INDEX_MAX 2
#define GUC_GENERIC_GT_SYSINFO_MAX 16
#define GUC_RENDER_CLASS 0
#define GUC_VIDEO_CLASS 1
#define GUC_VIDEOENHANCE_CLASS 2
#define GUC_BLITTER_CLASS 3
#define GUC_COMPUTE_CLASS 4

struct guc_mmio_reg_set {
	uint32_t address;
	uint16_t count;
	uint16_t reserved;
} __attribute__((packed));

struct guc_ads {
	struct guc_mmio_reg_set reg_state_list[GUC_MAX_ENGINE_CLASSES][GUC_MAX_INSTANCES_PER_CLASS];
	uint32_t reserved0;
	uint32_t scheduler_policies;
	uint32_t gt_system_info;
	uint32_t reserved1;
	uint32_t control_data;
	uint32_t golden_context_lrca[GUC_MAX_ENGINE_CLASSES];
	uint32_t eng_state_size[GUC_MAX_ENGINE_CLASSES];
	uint32_t private_data;
	uint32_t reserved2;
	uint32_t um_init_data;
	uint32_t reserved3;
	uint32_t capture_instance[GUC_CAPTURE_LIST_INDEX_MAX][GUC_MAX_ENGINE_CLASSES];
	uint32_t capture_class[GUC_CAPTURE_LIST_INDEX_MAX][GUC_MAX_ENGINE_CLASSES];
	uint32_t capture_global[GUC_CAPTURE_LIST_INDEX_MAX];
	uint32_t wa_klv_addr_lo;
	uint32_t wa_klv_addr_hi;
	uint32_t wa_klv_size;
	uint32_t reserved[11];
} __attribute__((packed));

struct guc_policies {
	uint32_t submission_queue_depth[GUC_MAX_ENGINE_CLASSES];
	uint32_t dpc_promote_time;
	uint32_t max_num_work_items;
	uint32_t global_flags;
	uint32_t is_valid;
	uint32_t reserved[22];
} __attribute__((packed));

struct guc_gt_system_info {
	uint8_t mapping_table[GUC_MAX_ENGINE_CLASSES][GUC_MAX_INSTANCES_PER_CLASS];
	uint32_t engine_enabled_masks[GUC_MAX_ENGINE_CLASSES];
	uint32_t generic_gt_sysinfo[GUC_GENERIC_GT_SYSINFO_MAX];
} __attribute__((packed));

struct guc_engine_usage_record {
	uint32_t current_context_index;
	uint32_t last_switch_in_stamp;
	uint32_t reserved0;
	uint32_t total_runtime;
	uint32_t reserved1[4];
} __attribute__((packed));

struct guc_ads_blob {
	struct guc_ads ads;
	struct guc_policies policies;
	struct guc_gt_system_info system_info;
	struct guc_engine_usage_record engine_usage[GUC_MAX_ENGINE_CLASSES][GUC_MAX_INSTANCES_PER_CLASS];
	uint32_t um_init_params[16];
} __attribute__((packed));

#define GUC_ADS_PRIVATE_DATA_BYTES (16 * 1024) /* the GuC's own scratch */
#define GUC_ADS_BLOB_PAGES ((sizeof(struct guc_ads_blob) + 4095) / 4096)

static int guc_class_of(int uapi_class)
{
	switch (uapi_class) {
	case I915_ENGINE_CLASS_RENDER: return GUC_RENDER_CLASS;
	case I915_ENGINE_CLASS_COPY: return GUC_BLITTER_CLASS;
	case I915_ENGINE_CLASS_VIDEO: return GUC_VIDEO_CLASS;
	case I915_ENGINE_CLASS_VIDEO_ENHANCE: return GUC_VIDEOENHANCE_CLASS;
	default: return GUC_COMPUTE_CLASS;
	}
}

/* The golden context per class comes from the kernel context's image
 * once the engines have run their first batch; before that (or where
 * the GuC loads before the engines) the entries stay empty and the
 * GuC starts contexts from what the host wrote. */
static uint32_t golden_bytes_for(struct i915_device *i915, int cls)
{
	for (int i = 0; i < I915_NUM_ENGINES; i++) {
		struct i915_engine *e = &i915->engines[i];
		if (!e->present || guc_class_of(e->class) != cls)
			continue;
		if (e->kctx && e->kctx->lrc[e->id].obj)
			return (uint32_t)e->kctx->lrc[e->id].obj->size;
	}
	return 0;
}

static int ads_create(struct i915_device *i915)
{
	struct i915_guc *g = &i915->guc;
	uint32_t golden_off[GUC_MAX_ENGINE_CLASSES];
	uint32_t total = (uint32_t)GUC_ADS_BLOB_PAGES * 4096 + GUC_ADS_PRIVATE_DATA_BYTES;
	uint32_t private_off = (uint32_t)GUC_ADS_BLOB_PAGES * 4096;

	for (int c = 0; c < GUC_MAX_ENGINE_CLASSES; c++) {
		uint32_t bytes = golden_bytes_for(i915, c);
		golden_off[c] = bytes ? total : 0;
		total += (bytes + 4095) & ~4095u;
	}
	g->ads_obj = i915_gem_alloc_bound(i915, total, 0, 1, &g->ads_ggtt);
	if (!g->ads_obj)
		return -ENOMEM;
	g->ads_vaddr = phys_to_virt(g->ads_obj->pages[0]);
	mm_memset(g->ads_vaddr, 0, total);
	struct guc_ads_blob *b = (struct guc_ads_blob *)g->ads_vaddr;
	uint32_t base = g->ads_ggtt;

	/* the register lists: none (nothing for the GuC to restore after
	 * an engine reset beyond the context image) */
	for (int c = 0; c < GUC_MAX_ENGINE_CLASSES; c++)
		for (int n = 0; n < GUC_MAX_INSTANCES_PER_CLASS; n++) {
			b->ads.reg_state_list[c][n].address = 0;
			b->ads.reg_state_list[c][n].count = 0;
		}
	b->ads.scheduler_policies = base + (uint32_t)__builtin_offsetof(struct guc_ads_blob, policies);
	b->ads.gt_system_info = base + (uint32_t)__builtin_offsetof(struct guc_ads_blob, system_info);
	b->ads.private_data = base + private_off;
	b->ads.um_init_data = base + (uint32_t)__builtin_offsetof(struct guc_ads_blob, um_init_params);
	/* the policies: the firmware's defaults, marked valid */
	for (int c = 0; c < GUC_MAX_ENGINE_CLASSES; c++)
		b->policies.submission_queue_depth[c] = 1;
	b->policies.dpc_promote_time = 500000;
	b->policies.max_num_work_items = 15;
	b->policies.global_flags = 0;
	b->policies.is_valid = 1;
	/* the engines: which exist, and the instance behind each logical one */
	for (int c = 0; c < GUC_MAX_ENGINE_CLASSES; c++)
		for (int n = 0; n < GUC_MAX_INSTANCES_PER_CLASS; n++)
			b->system_info.mapping_table[c][n] = GUC_MAX_INSTANCES_PER_CLASS;
	/* from the device table: on a part that submits through the GuC
	 * the engines are not up yet when this is written */
	static const struct {
		uint32_t bit;
		int cls, inst;
	} table[] = {
		{ I915_ENGINE_RCS0, GUC_RENDER_CLASS, 0 },
		{ I915_ENGINE_BCS0, GUC_BLITTER_CLASS, 0 },
		{ I915_ENGINE_VCS0, GUC_VIDEO_CLASS, 0 },
		{ I915_ENGINE_VCS1, GUC_VIDEO_CLASS, 1 },
		{ I915_ENGINE_VCS2, GUC_VIDEO_CLASS, 2 },
		{ I915_ENGINE_VCS3, GUC_VIDEO_CLASS, 3 },
		{ I915_ENGINE_VECS0, GUC_VIDEOENHANCE_CLASS, 0 },
		{ I915_ENGINE_VECS1, GUC_VIDEOENHANCE_CLASS, 1 },
		{ I915_ENGINE_CCS0, GUC_COMPUTE_CLASS, 0 },
		{ I915_ENGINE_CCS1, GUC_COMPUTE_CLASS, 1 },
		{ I915_ENGINE_CCS2, GUC_COMPUTE_CLASS, 2 },
		{ I915_ENGINE_CCS3, GUC_COMPUTE_CLASS, 3 },
	};
	for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
		if (!(i915->info->engine_mask & table[i].bit))
			continue;
		b->system_info.engine_enabled_masks[table[i].cls] |= 1u << table[i].inst;
		b->system_info.mapping_table[table[i].cls][table[i].inst] = (uint8_t)table[i].inst;
	}
	b->system_info.generic_gt_sysinfo[0] = 1; /* slices enabled: one is a safe floor */
	b->system_info.generic_gt_sysinfo[1] = 0; /* video boxes with SFC: none claimed */
	b->system_info.generic_gt_sysinfo[2] = 256; /* doorbells per SQIDI */
	/* the golden contexts */
	for (int c = 0; c < GUC_MAX_ENGINE_CLASSES; c++) {
		if (!golden_off[c])
			continue;
		for (int i = 0; i < I915_NUM_ENGINES; i++) {
			struct i915_engine *e = &i915->engines[i];
			if (!e->present || guc_class_of(e->class) != c || !e->kctx)
				continue;
			struct drm_gem_object *o = e->kctx->lrc[e->id].obj;
			if (!o)
				continue;
			for (uint32_t p = 0; p < o->npages; p++)
				mm_memcpy(g->ads_vaddr + golden_off[c] + p * 4096,
					  phys_to_virt(o->pages[p]), 4096);
			b->ads.golden_context_lrca[c] = base + golden_off[c];
			/* the state after the per-process status page */
			b->ads.eng_state_size[c] = (uint32_t)o->size - 4096;
			break;
		}
	}
	__asm__ volatile("mfence" ::: "memory");
	return 0;
}

/* ---- the WOPCM partition ---------------------------------------------------------- */

#define WOPCM_RESERVED_SIZE (16 * 1024)
#define GUC_WOPCM_RESERVED (16 * 1024)
#define GUC_WOPCM_STACK_RESERVED (8 * 1024)
#define GUC_WOPCM_OFFSET_ALIGNMENT (16 * 1024)
#define BXT_WOPCM_RC6_CTX_RESERVED (24 * 1024)
#define ICL_WOPCM_HW_CTX_RESERVED (36 * 1024)
#define GEN9_GUC_WOPCM_OFFSET 0x24000

static int wopcm_init(struct i915_device *i915, uint32_t guc_fw_size, uint32_t huc_fw_size)
{
	struct i915_guc *g = &i915->guc;
	uint32_t wopcm = i915->info->gen_x10 >= 125 ? 4u << 20 :
			 i915->info->gen >= 11	     ? 2u << 20 :
						       1u << 20;
	uint32_t ctx_rsvd = (i915->info->flags & I915_INFO_IS_LP) && i915->info->gen == 9 ?
				    BXT_WOPCM_RC6_CTX_RESERVED :
			    i915->info->gen >= 11 ? ICL_WOPCM_HW_CTX_RESERVED :
						    0;
	uint32_t size_reg = i915_read32(i915, GUC_WOPCM_SIZE);
	uint32_t off_reg = i915_read32(i915, DMA_GUC_WOPCM_OFFSET);
	uint32_t base, size;

	if ((size_reg & GUC_WOPCM_SIZE_LOCKED) || (off_reg & GUC_WOPCM_OFFSET_VALID)) {
		/* the firmware partitioned it already, and locked it */
		base = off_reg & GUC_WOPCM_OFFSET_MASK;
		size = size_reg & GUC_WOPCM_SIZE_MASK;
		i915_dbg("[drm] i915: WOPCM partitioned by the firmware: GuC at %x, %u KB\n", base,
			 size >> 10);
	} else {
		base = (huc_fw_size + WOPCM_RESERVED_SIZE + GUC_WOPCM_OFFSET_ALIGNMENT - 1) &
		       ~(GUC_WOPCM_OFFSET_ALIGNMENT - 1);
		if (base + ctx_rsvd >= wopcm)
			return -E2BIG;
		size = (wopcm - ctx_rsvd - base) & GUC_WOPCM_SIZE_MASK;
	}
	if (guc_fw_size + GUC_WOPCM_RESERVED + GUC_WOPCM_STACK_RESERVED > size) {
		kprintf("[drm] i915: GuC firmware (%u KB) does not fit its WOPCM share (%u KB)\n",
			guc_fw_size >> 10, size >> 10);
		return -E2BIG;
	}
	if (i915->info->gen == 9) {
		uint32_t off = base + GEN9_GUC_WOPCM_OFFSET;
		if (off > size || size - off < 4) {
			kprintf("[drm] i915: WOPCM layout fails the Gen9 dword-gap rule\n");
			return -E2BIG;
		}
		if (huc_fw_size + WOPCM_RESERVED_SIZE > base) {
			kprintf("[drm] i915: HuC firmware does not fit below the GuC's WOPCM share\n");
			return -E2BIG;
		}
	}
	g->wopcm_base = base;
	g->wopcm_size = size;
	if (!(size_reg & GUC_WOPCM_SIZE_LOCKED)) {
		i915_write32(i915, GUC_WOPCM_SIZE, size | GUC_WOPCM_SIZE_LOCKED);
		i915_write32(i915, DMA_GUC_WOPCM_OFFSET,
			     base | (huc_fw_size ? HUC_LOADING_AGENT_GUC : 0) | GUC_WOPCM_OFFSET_VALID);
		(void)i915_read32(i915, DMA_GUC_WOPCM_OFFSET);
	}
	return 0;
}

/* ---- the upload --------------------------------------------------------------------- */

static int guc_reset(struct i915_device *i915)
{
	uint32_t dom = i915->info->gen >= 11 ? GEN11_GRDOM_GUC : GEN9_GRDOM_GUC;
	i915_fw_get(i915, I915_FW_ALL);
	i915_write32_fw(i915, GEN6_GDRST, dom);
	int rc = 0;
	for (int t = 0;; t++) {
		if (!(i915_read32_fw(i915, GEN6_GDRST) & dom))
			break;
		if (t > 5000) {
			rc = -ETIMEDOUT;
			break;
		}
		lapic_delay_us(10);
	}
	i915_fw_put(i915, I915_FW_ALL);
	if (rc)
		kprintf("[drm] i915: the GuC did not come out of reset\n");
	return rc;
}

static void guc_prepare_xfer(struct i915_device *i915)
{
	uint32_t shim = GUC_ENABLE_READ_CACHE_LOGIC | GUC_ENABLE_READ_CACHE_FOR_SRAM_DATA |
			GUC_ENABLE_READ_CACHE_FOR_WOPCM_DATA | GUC_ENABLE_MIA_CLOCK_GATING;
	if (i915->info->gen_x10 < 125)
		shim |= GUC_DISABLE_SRAM_INIT_TO_ZEROES | GUC_ENABLE_MIA_CACHING;
	if (i915->info->gen == 9)
		shim |= GUC_GEN10_MSGCH_ENABLE;
	i915_write32(i915, GUC_SHIM_CONTROL, shim);
	if ((i915->info->flags & I915_INFO_IS_LP) && i915->info->gen == 9)
		i915_write32(i915, GEN9LP_GT_PM_CONFIG, GT_DOORBELL_ENABLE);
	else
		i915_write32(i915, GEN9_GT_PM_CONFIG, GT_DOORBELL_ENABLE);
	if (i915->info->gen == 9) {
		uint32_t v = i915_read32(i915, GEN7_MISCCPCTL);
		i915_write32(i915, GEN7_MISCCPCTL, v | GEN8_DOP_CLOCK_GATE_GUC_ENABLE);
		i915_write32(i915, GUC_ARAT_C6DIS, 0x1ff); /* 5 us before RC6 */
	}
}

/* The DMA of an image (header and code) into the WOPCM. */
static int uc_dma(struct i915_device *i915, uint32_t src_ggtt, uint32_t bytes, uint32_t dst,
		  uint32_t flags)
{
	i915_fw_get(i915, I915_FW_ALL);
	i915_write32_fw(i915, DMA_ADDR_0_LOW, src_ggtt);
	i915_write32_fw(i915, DMA_ADDR_0_HIGH, 0);
	i915_write32_fw(i915, DMA_ADDR_1_LOW, dst);
	i915_write32_fw(i915, DMA_ADDR_1_HIGH, DMA_ADDRESS_SPACE_WOPCM);
	i915_write32_fw(i915, DMA_COPY_SIZE, bytes);
	i915_write32_fw(i915, DMA_CTRL, I915_MASKED_ENABLE(flags | START_DMA));
	int rc = -ETIMEDOUT;
	for (int t = 0; t < 10000; t++) {
		if (!(i915_read32_fw(i915, DMA_CTRL) & START_DMA)) {
			rc = 0;
			break;
		}
		lapic_delay_us(10);
	}
	i915_write32_fw(i915, DMA_CTRL, I915_MASKED_DISABLE(flags));
	i915_fw_put(i915, I915_FW_ALL);
	return rc;
}

/* The GuC's signature goes in through the scratch registers. */
static void guc_rsa_to_regs(struct i915_device *i915, const uint8_t *rsa)
{
	for (int i = 0; i < UOS_RSA_SCRATCH_COUNT; i++)
		i915_write32(i915, UOS_RSA_SCRATCH(i), rd32(rsa + i * 4));
}

#define GUC_CTL_LOG_PARAMS 0
#define GUC_CTL_FEATURE 2
#define GUC_CTL_DEBUG 3
#define GUC_CTL_ADS 4
#define GUC_CTL_WA 5
#define GUC_CTL_DEVID 6
#define GUC_CTL_MAX_DWORDS 14
#define GUC_CTL_DISABLE_SCHEDULER (1u << 14)
#define GUC_LOG_DISABLED (1u << 6)
#define GUC_ADS_ADDR_SHIFT 1

static void guc_write_params(struct i915_device *i915)
{
	struct i915_guc *g = &i915->guc;
	uint32_t p[GUC_CTL_MAX_DWORDS];
	mm_memset(p, 0, sizeof(p));
	p[GUC_CTL_LOG_PARAMS] = 0; /* no log buffer */
	p[GUC_CTL_FEATURE] = g->submission ? 0 : GUC_CTL_DISABLE_SCHEDULER;
	p[GUC_CTL_DEBUG] = GUC_LOG_DISABLED;
	p[GUC_CTL_ADS] = (g->ads_ggtt >> 12) << GUC_ADS_ADDR_SHIFT;
	p[GUC_CTL_WA] = 0;
	p[GUC_CTL_DEVID] = ((uint32_t)i915->devid << 16) | i915->revid;
	i915_write32(i915, SOFT_SCRATCH(0), 0);
	for (int i = 0; i < GUC_CTL_MAX_DWORDS; i++)
		i915_write32(i915, SOFT_SCRATCH(1 + i), p[i]);
}

static int guc_wait_ready(struct i915_device *i915)
{
	uint32_t st = 0;
	for (int t = 0; t < 50000; t++) { /* half a second */
		st = i915_read32(i915, GUC_STATUS);
		uint32_t uk = (st & GS_UKERNEL_MASK) >> GS_UKERNEL_SHIFT;
		if (uk == GUC_LOAD_STATUS_READY)
			return 0;
		if ((st & GS_BOOTROM_MASK) == GS_BOOTROM_RSA_FAILED) {
			kprintf("[drm] i915: GuC firmware signature rejected (status %08x)\n", st);
			return -EACCES;
		}
		if (uk >= 0x70 && uk < 0x80 && uk != GUC_LOAD_STATUS_READY && t > 100) {
			/* the exception range: the kernel started and fell over */
			kprintf("[drm] i915: GuC firmware failed to start (status %08x)\n", st);
			return -EIO;
		}
		lapic_delay_us(10);
	}
	kprintf("[drm] i915: GuC firmware did not become ready (status %08x)\n", st);
	return -ETIMEDOUT;
}

static int guc_upload(struct i915_device *i915, const struct uc_image *img)
{
	struct i915_guc *g = &i915->guc;
	const uint8_t *css = img->data;
	guc_prepare_xfer(i915);
	guc_rsa_to_regs(i915, css + UC_CSS_HEADER_BYTES + img->ucode_size);
	/* the code lands at 8 KB into the GuC's WOPCM share */
	int rc = uc_dma(i915, g->guc_ggtt, UC_CSS_HEADER_BYTES + img->ucode_size, 0x2000, UOS_MOVE);
	if (rc) {
		kprintf("[drm] i915: GuC firmware DMA did not finish\n");
		return rc;
	}
	return guc_wait_ready(i915);
}

static int huc_upload(struct i915_device *i915, const struct uc_image *img)
{
	struct i915_guc *g = &i915->guc;
	int rc = uc_dma(i915, g->huc_ggtt, UC_CSS_HEADER_BYTES + img->ucode_size, 0, HUC_UKERNEL);
	if (rc)
		kprintf("[drm] i915: HuC firmware DMA did not finish\n");
	return rc;
}

static int huc_auth(struct i915_device *i915)
{
	struct i915_guc *g = &i915->guc;
	uint32_t data[1] = { g->huc_rsa_ggtt };
	uint32_t resp[1];
	int rc = guc_send_mmio(i915, GUC_ACTION_AUTHENTICATE_HUC, data, 1, resp, 1);
	if (rc)
		return rc;
	uint32_t reg = i915->info->gen >= 11 ? GEN11_HUC_KERNEL_LOAD_INFO : HUC_STATUS2;
	uint32_t bit = i915->info->gen >= 11 ? HUC_LOAD_SUCCESSFUL : HUC_FW_VERIFIED;
	if (wait_reg(i915, reg, bit, bit, 50)) {
		kprintf("[drm] i915: HuC authentication did not complete (%08x)\n",
			i915_read32(i915, reg));
		return -ETIMEDOUT;
	}
	g->huc_authenticated = 1;
	return 0;
}

/* ---- the command transport --------------------------------------------------------- */

/* One page of descriptors, then the two rings. */
#define CT_DESC_H2G 0
#define CT_DESC_G2H 64
#define CT_H2G_OFF 4096
#define CT_H2G_BYTES (16 * 1024)
#define CT_G2H_OFF (CT_H2G_OFF + CT_H2G_BYTES)
#define CT_G2H_BYTES (16 * 1024)
#define CT_TOTAL (CT_G2H_OFF + CT_G2H_BYTES)

struct guc_ct_buffer_desc {
	uint32_t head; /* in dwords */
	uint32_t tail;
	uint32_t status;
	uint32_t reserved[13];
} __attribute__((packed));

#define CTB_MSG_0_NUM_DWORDS_MASK 0xffu
#define CTB_MSG_0_FORMAT_HXG (0u << 12)
#define CTB_MSG_0_FENCE(f) ((uint32_t)(f) << 16)
#define CTB_MSG_0_FENCE_OF(h) ((h) >> 16)

static volatile struct guc_ct_buffer_desc *ct_desc(struct i915_device *i915, int g2h)
{
	return (volatile struct guc_ct_buffer_desc *)(i915->guc.ct_vaddr +
						      (g2h ? CT_DESC_G2H : CT_DESC_H2G));
}

static volatile uint32_t *ct_ring(struct i915_device *i915, int g2h)
{
	return (volatile uint32_t *)(i915->guc.ct_vaddr + (g2h ? CT_G2H_OFF : CT_H2G_OFF));
}

static int ct_create(struct i915_device *i915)
{
	struct i915_guc *g = &i915->guc;
	g->ct_obj = i915_gem_alloc_bound(i915, CT_TOTAL, 1, 1, &g->ct_ggtt);
	if (!g->ct_obj)
		return -ENOMEM;
	g->ct_vaddr = phys_to_virt(g->ct_obj->pages[0]);
	mm_memset(g->ct_vaddr, 0, CT_TOTAL);
	g->ct_h2g_size = CT_H2G_BYTES / 4;
	g->ct_g2h_size = CT_G2H_BYTES / 4;
	spinlock_init(&g->ct_lock, "i915_guc_ct");
	__asm__ volatile("mfence" ::: "memory");
	return 0;
}

static int self_cfg(struct i915_device *i915, uint32_t key, uint32_t len, uint64_t value)
{
	uint32_t data[3] = { KLV(key, len), (uint32_t)value, (uint32_t)(value >> 32) };
	uint32_t resp[1];
	int rc = guc_send_mmio(i915, GUC_ACTION_HOST2GUC_SELF_CFG, data, 3, resp, 1);
	if (rc)
		return rc;
	/* the answer's data says whether the key was taken */
	if (HXG_RESPONSE_DATA0(resp[0]) != 1) {
		kprintf("[drm] i915: GuC did not take configuration key %04x\n", key);
		return -EIO;
	}
	return 0;
}

static int ct_enable(struct i915_device *i915)
{
	struct i915_guc *g = &i915->guc;
	int rc;
	rc = self_cfg(i915, KLV_SELF_CFG_H2G_CTB_DESC_ADDR, 2, g->ct_ggtt + CT_DESC_H2G);
	if (!rc)
		rc = self_cfg(i915, KLV_SELF_CFG_H2G_CTB_ADDR, 2, g->ct_ggtt + CT_H2G_OFF);
	if (!rc)
		rc = self_cfg(i915, KLV_SELF_CFG_H2G_CTB_SIZE, 1, CT_H2G_BYTES);
	if (!rc)
		rc = self_cfg(i915, KLV_SELF_CFG_G2H_CTB_DESC_ADDR, 2, g->ct_ggtt + CT_DESC_G2H);
	if (!rc)
		rc = self_cfg(i915, KLV_SELF_CFG_G2H_CTB_ADDR, 2, g->ct_ggtt + CT_G2H_OFF);
	if (!rc)
		rc = self_cfg(i915, KLV_SELF_CFG_G2H_CTB_SIZE, 1, CT_G2H_BYTES);
	if (rc)
		return rc;
	uint32_t data[1] = { GUC_CTB_CONTROL_ENABLE };
	uint32_t resp[1];
	rc = guc_send_mmio(i915, GUC_ACTION_HOST2GUC_CONTROL_CTB, data, 1, resp, 1);
	if (rc)
		return rc;
	g->comm = 1;
	i915_guc_irq_enable(i915);
	return 0;
}

/* A message into the host-to-GuC ring, or -EBUSY when it is full.  A
 * "fast request" gets no answer beyond the events the action itself
 * produces; the callers here want none.  Caller holds ct_lock. */
static int ct_send(struct i915_device *i915, uint32_t action, const uint32_t *data, int ndata)
{
	struct i915_guc *g = &i915->guc;
	volatile struct guc_ct_buffer_desc *d = ct_desc(i915, 0);
	volatile uint32_t *ring = ct_ring(i915, 0);
	uint32_t size = g->ct_h2g_size;
	uint32_t head = d->head, tail = d->tail;
	uint32_t len = 2 + (uint32_t)ndata; /* header, HXG, data */

	if (d->status) {
		if (!(g->send_failures++))
			kprintf("[drm] i915: GuC transport reports status %x\n", d->status);
		return -EIO;
	}
	if (head >= size || tail >= size)
		return -EIO;
	uint32_t space = (head <= tail) ? size - (tail - head) : head - tail;
	if (space <= len + 1)
		return -EBUSY;
	/* the message must not wrap: pad to the end with a null message */
	if (tail + len > size) {
		uint32_t pad = size - tail;
		if (space <= len + pad + 1)
			return -EBUSY;
		ring[tail] = CTB_MSG_0_FORMAT_HXG | ((pad - 1) & CTB_MSG_0_NUM_DWORDS_MASK);
		for (uint32_t i = 1; i < pad; i++)
			ring[tail + i] = 0;
		tail = 0;
	}
	uint16_t fence = ++g->ct_fence;
	ring[tail] = CTB_MSG_0_FENCE(fence) | CTB_MSG_0_FORMAT_HXG |
		     ((len - 1) & CTB_MSG_0_NUM_DWORDS_MASK);
	ring[tail + 1] = (HXG_TYPE_FAST_REQUEST << HXG_TYPE_SHIFT) | action;
	for (int i = 0; i < ndata; i++)
		ring[tail + 2 + i] = data[i];
	__asm__ volatile("mfence" ::: "memory");
	d->tail = (tail + len) % size;
	__asm__ volatile("mfence" ::: "memory");
	guc_notify(i915);
	return 0;
}

static void g2h_event(struct i915_device *i915, uint32_t action, const uint32_t *data, int n)
{
	struct i915_guc *g = &i915->guc;
	g->g2h_events++;
	switch (action) {
	case GUC2HOST_SCHED_CONTEXT_MODE_DONE:
		break;
	case GUC2HOST_DEREGISTER_CONTEXT_DONE:
		if (n >= 1 && data[0] < I915_GUC_MAX_CONTEXTS)
			g->ctx_state[data[0]] = 0;
		break;
	case GUC2HOST_CONTEXT_RESET_NOTIFICATION:
		g->resets_seen++;
		kprintf("[drm] i915: GuC reset context %u\n", n >= 1 ? data[0] : 0);
		break;
	case GUC2HOST_ENGINE_FAILURE_NOTIFICATION:
		g->resets_seen++;
		kprintf("[drm] i915: GuC reports an engine failure (class %u instance %u, reason %x)\n",
			n >= 1 ? data[0] : 0, n >= 2 ? data[1] : 0, n >= 3 ? data[2] : 0);
		break;
	case GUC2HOST_NOTIFY_CRASH_DUMP:
	case GUC2HOST_NOTIFY_EXCEPTION:
		kprintf("[drm] i915: the GuC crashed (event %04x)\n", action);
		break;
	case GUC2HOST_NOTIFY_FLUSH_LOG:
		break;
	default:
		g->g2h_unknown++;
		break;
	}
}

void i915_guc_ct_process(struct i915_device *i915)
{
	struct i915_guc *g = &i915->guc;
	uint64_t fl;
	if (!g->comm)
		return;
	spin_lock_irqsave(&g->ct_lock, &fl);
	volatile struct guc_ct_buffer_desc *d = ct_desc(i915, 1);
	volatile uint32_t *ring = ct_ring(i915, 1);
	uint32_t size = g->ct_g2h_size;
	uint32_t head = d->head, tail = d->tail;
	int guard = 0;
	while (head != tail && guard++ < 4096) {
		if (head >= size || tail >= size)
			break;
		uint32_t hdr = ring[head];
		uint32_t len = (hdr & CTB_MSG_0_NUM_DWORDS_MASK) + 1;
		if (len > size || head + len > size) {
			/* a message that would wrap is a padding message */
			head = 0;
			continue;
		}
		if (len >= 2) {
			uint32_t hxg = ring[head + 1];
			uint32_t type = (hxg & HXG_TYPE_MASK) >> HXG_TYPE_SHIFT;
			uint32_t data[8];
			int n = (int)len - 2;
			if (n > 8)
				n = 8;
			for (int i = 0; i < n; i++)
				data[i] = ring[head + 2 + i];
			if (type == HXG_TYPE_EVENT)
				g2h_event(i915, hxg & HXG_ACTION_MASK, data, n);
			else if (type == HXG_TYPE_RESPONSE_FAILURE)
				kprintf("[drm] i915: GuC failed request %u: error %x, hint %x\n",
					CTB_MSG_0_FENCE_OF(hdr), HXG_FAILURE_ERROR(hxg),
					HXG_FAILURE_HINT(hxg));
		}
		head = (head + len) % size;
	}
	__asm__ volatile("mfence" ::: "memory");
	d->head = head;
	spin_unlock_irqrestore(&g->ct_lock, fl);
}

void i915_guc_irq(struct i915_device *i915)
{
	/* the messages are read by the GT worker, off the interrupt */
	if (i915->gt_worker_ready)
		poll_notify_wq(&i915->gt_wq);
	i915->hangcheck_pending = 1;
}

void i915_guc_irq_enable(struct i915_device *i915)
{
	if (i915->info->gen >= 11) {
		i915_write32(i915, GEN11_GUC_SG_INTR_ENABLE, GUC_INTR_GUC2HOST << 16);
		i915_write32(i915, GEN11_GUC_SG_INTR_MASK, ~(GUC_INTR_GUC2HOST << 16));
		return;
	}
	uint32_t bits = GUC_INTR_GUC2HOST << 16;
	i915_write32_fw(i915, GEN8_GT_IIR(2), bits);
	i915_write32_fw(i915, GEN8_GT_IMR(2), i915_read32_fw(i915, GEN8_GT_IMR(2)) & ~bits);
	i915_write32_fw(i915, GEN8_GT_IER(2), i915_read32_fw(i915, GEN8_GT_IER(2)) | bits);
	(void)i915_read32_fw(i915, GEN8_GT_IER(2));
}

/* ---- submission ------------------------------------------------------------------ */

static int ctx_id_alloc(struct i915_guc *g)
{
	for (uint32_t n = 0; n < I915_GUC_MAX_CONTEXTS; n++) {
		uint32_t id = (g->ctx_next + n) % I915_GUC_MAX_CONTEXTS;
		if (id == 0)
			continue; /* 0 is left alone */
		if (!g->ctx_state[id]) {
			g->ctx_state[id] = 1;
			g->ctx_next = id + 1;
			return (int)id;
		}
	}
	return -1;
}

/* Register the logical ring with the GuC and set its policies.  Caller
 * holds ct_lock. */
static int lrc_register(struct i915_device *i915, struct i915_engine *e, struct i915_lrc *lrc)
{
	struct i915_guc *g = &i915->guc;
	if (!lrc->guc_id) {
		int id = ctx_id_alloc(g);
		if (id < 0)
			return -ENOSPC;
		lrc->guc_id = (uint32_t)id;
	}
	i915_guc_ggtt_invalidate(i915);
	uint32_t reg[11] = {
		CONTEXT_REGISTRATION_FLAG_KMD,
		lrc->guc_id,
		(uint32_t)guc_class_of(e->class),
		1u << e->instance,
		0, 0, 0, 0, 0, /* no work queue: a single logical ring */
		lrc->ggtt,
		0,
	};
	int rc = ct_send(i915, GUC_ACTION_REGISTER_CONTEXT, reg, 11);
	if (rc)
		return rc;
	uint32_t pol[9] = {
		lrc->guc_id,
		KLV(KLV_CTX_EXECUTION_QUANTUM, 2), 1000, 0, /* 1 ms, in microseconds */
		KLV(KLV_CTX_PREEMPTION_TIMEOUT, 2), 640000, 0, /* 640 ms */
		KLV(KLV_CTX_SCHEDULING_PRIORITY, 1), GUC_CLIENT_PRIORITY_KMD_NORMAL,
	};
	rc = ct_send(i915, GUC_ACTION_UPDATE_CONTEXT_POLICIES, pol, 9);
	if (rc)
		return rc; /* registered; the policies go again next time */
	lrc->guc_registered = 1;
	return 0;
}

/* Caller holds e->lock: hand every queued request to the GuC.  Unlike
 * the execution lists there is no port to be busy: the GuC schedules
 * among the contexts itself.  A full transport leaves the rest queued;
 * the GT worker calls again. */
void i915_guc_submit(struct i915_engine *e)
{
	struct i915_device *i915 = e->i915;
	struct i915_guc *g = &i915->guc;
	uint64_t fl;

	while (e->queue) {
		struct i915_request *rq = e->queue;
		struct i915_lrc *lrc = rq->lrc;
		if (!lrc || !lrc->obj)
			return;
		uint32_t tail = rq->ring_tail;
		struct i915_request *last = rq;
		while (last->next && last->next->lrc == lrc) {
			last = last->next;
			tail = last->ring_tail;
		}
		struct i915_lrc_layout l;
		i915_lrc_layout(i915->info->gen_x10, e->class, &l);
		lrc->regs[l.ring_tail + 1] = tail;
		if (!(i915->info->flags & I915_INFO_HAS_LLC)) {
			uint8_t *va = (uint8_t *)lrc->regs;
			for (int i = 0; i < 4096; i += 64)
				__asm__ volatile("clflush (%0)" ::"r"(va + i) : "memory");
		}
		__asm__ volatile("mfence" ::: "memory");

		spin_lock_irqsave(&g->ct_lock, &fl);
		int rc = 0;
		if (!lrc->guc_registered)
			rc = lrc_register(i915, e, lrc);
		if (!rc) {
			if (!lrc->guc_enabled) {
				uint32_t d[2] = { lrc->guc_id, GUC_CONTEXT_ENABLE };
				rc = ct_send(i915, GUC_ACTION_SCHED_CONTEXT_MODE_SET, d, 2);
				if (!rc)
					lrc->guc_enabled = 1;
			} else {
				uint32_t d[1] = { lrc->guc_id };
				rc = ct_send(i915, GUC_ACTION_SCHED_CONTEXT, d, 1);
			}
		}
		spin_unlock_irqrestore(&g->ct_lock, fl);
		if (rc) {
			if (rc != -EBUSY && !(g->send_failures++))
				kprintf("[drm] i915: %s: GuC submission failed (%d)\n", e->name, rc);
			return; /* stays queued */
		}
		/* [rq..last] are with the hardware now */
		e->queue = last->next;
		if (!e->queue)
			e->queue_tail = NULL;
		last->next = NULL;
		if (e->inflight_tail)
			e->inflight_tail->next = rq;
		else
			e->inflight = rq;
		e->inflight_tail = last;
		for (struct i915_request *r = rq; r; r = r->next) {
			r->submitted = 1;
			r->submitted_ns = hrtimer_now_ns();
		}
		e->active_ctx = rq->ctx;
		e->active_lrc = lrc;
		e->active_ctx_id = lrc->hw_id;
	}
}

/* A logical ring going away: scheduling off and the registration
 * dropped; the id is reused once the GuC says it has let go. */
void i915_guc_lrc_release(struct i915_device *i915, struct i915_lrc *lrc)
{
	struct i915_guc *g = &i915->guc;
	uint64_t fl;
	if (!g->submission || !lrc->guc_id)
		return;
	spin_lock_irqsave(&g->ct_lock, &fl);
	if (lrc->guc_registered) {
		uint32_t d[2] = { lrc->guc_id, GUC_CONTEXT_DISABLE };
		if (lrc->guc_enabled)
			(void)ct_send(i915, GUC_ACTION_SCHED_CONTEXT_MODE_SET, d, 2);
		if (ct_send(i915, GUC_ACTION_DEREGISTER_CONTEXT, d, 1) == 0)
			g->ctx_state[lrc->guc_id] = 2; /* until the GuC confirms */
		else
			g->ctx_state[lrc->guc_id] = 0;
	} else {
		g->ctx_state[lrc->guc_id] = 0;
	}
	lrc->guc_id = 0;
	lrc->guc_registered = 0;
	lrc->guc_enabled = 0;
	spin_unlock_irqrestore(&g->ct_lock, fl);
}

int i915_guc_submission_enable(struct i915_device *i915)
{
	struct i915_guc *g = &i915->guc;
	if (!g->loaded || !g->comm)
		return -ENODEV;
	g->submission = 1;
	kprintf("[drm] i915: submission through the GuC\n");
	return 0;
}

/* ---- policy and bring-up ----------------------------------------------------------- */

/* Which parts take the firmware: those that cannot submit without it,
 * and the Gen11/12 parts designed to run it.  Gen9 keeps the execution
 * lists alone -- the one platform this driver has been run on. */
int i915_guc_wants_load(struct i915_device *i915)
{
	if (!(i915->info->flags & I915_INFO_HAS_GUC) || !i915->info->guc_fw)
		return 0;
	if (i915->info->flags & I915_INFO_GUC_MANDATORY)
		return 1;
	return i915->info->gen >= 11;
}

int i915_guc_init(struct i915_device *i915)
{
	struct i915_guc *g = &i915->guc;
	struct uc_image guc_img, huc_img;
	int have_huc = 0;
	int rc;

	if (!i915_guc_wants_load(i915))
		return 0;
	g->wanted = 1;
	g->submission = !!(i915->info->flags & I915_INFO_GUC_MANDATORY);
	if (uc_image_open("GuC", i915->info->guc_fw, &guc_img))
		return -ENOENT;
	if (i915->info->huc_fw && uc_image_open("HuC", i915->info->huc_fw, &huc_img) == 0)
		have_huc = 1;
	g->fw_version = guc_img.version;
	g->guc_ucode_size = guc_img.ucode_size;
	if (have_huc) {
		g->huc_version = huc_img.version;
		g->huc_ucode_size = huc_img.ucode_size;
	}
	rc = wopcm_init(i915, UC_CSS_HEADER_BYTES + guc_img.ucode_size,
			have_huc ? UC_CSS_HEADER_BYTES + huc_img.ucode_size : 0);
	if (rc)
		goto out;
	g->guc_obj = image_to_ggtt(i915, &guc_img, &g->guc_ggtt);
	if (!g->guc_obj) {
		rc = -ENOMEM;
		goto out;
	}
	if (have_huc) {
		g->huc_obj = image_to_ggtt(i915, &huc_img, &g->huc_ggtt);
		g->huc_rsa_obj = i915_gem_alloc_bound(i915, 4096, 0, 1, &g->huc_rsa_ggtt);
		if (!g->huc_obj || !g->huc_rsa_obj) {
			rc = -ENOMEM;
			goto out;
		}
		mm_memcpy(phys_to_virt(g->huc_rsa_obj->pages[0]),
			  (const uint8_t *)huc_img.data + UC_CSS_HEADER_BYTES + huc_img.ucode_size,
			  huc_img.rsa_size);
	}
	rc = ads_create(i915);
	if (rc)
		goto out;
	rc = ct_create(i915);
	if (rc)
		goto out;
	__asm__ volatile("mfence" ::: "memory");
	i915_guc_ggtt_invalidate(i915);

	/* Gen9 wants up to three tries at the load; one is the norm. */
	int attempts = i915->info->gen == 9 ? 3 : 1;
	rc = -EIO;
	while (attempts-- > 0) {
		if (guc_reset(i915))
			continue;
		if (have_huc && huc_upload(i915, &huc_img) == 0)
			g->huc_loaded = 1;
		guc_write_params(i915);
		rc = guc_upload(i915, &guc_img);
		if (rc == 0)
			break;
	}
	if (rc)
		goto out;
	g->loaded = 1;
	kprintf("[drm] i915: GuC firmware %u.%u.%u running%s\n", (g->fw_version >> 16) & 0xff,
		(g->fw_version >> 8) & 0xff, g->fw_version & 0xff,
		g->submission ? "" : " (submission stays with the execution lists)");
	rc = ct_enable(i915);
	if (rc) {
		kprintf("[drm] i915: GuC command transport did not open (%d)\n", rc);
		goto out;
	}
	if (g->huc_loaded) {
		if (huc_auth(i915) == 0)
			kprintf("[drm] i915: HuC firmware %u.%u.%u authenticated\n",
				(g->huc_version >> 16) & 0xff, (g->huc_version >> 8) & 0xff,
				g->huc_version & 0xff);
	}
	rc = 0;
out:
	firmware_release(guc_img.data);
	if (have_huc)
		firmware_release(huc_img.data);
	if (rc && !g->loaded)
		kprintf("[drm] i915: GuC not loaded (%d)%s\n", rc,
			g->submission ? "; the GT stays off" : "; execution lists carry on");
	return rc;
}

void i915_guc_fini(struct i915_device *i915)
{
	struct i915_guc *g = &i915->guc;
	g->comm = 0;
	g->submission = 0;
	if (g->ct_obj)
		drm_gem_put(g->ct_obj);
	if (g->ads_obj)
		drm_gem_put(g->ads_obj);
	if (g->huc_rsa_obj)
		drm_gem_put(g->huc_rsa_obj);
	if (g->huc_obj)
		drm_gem_put(g->huc_obj);
	if (g->guc_obj)
		drm_gem_put(g->guc_obj);
	g->ct_obj = g->ads_obj = g->huc_rsa_obj = g->huc_obj = g->guc_obj = NULL;
}
