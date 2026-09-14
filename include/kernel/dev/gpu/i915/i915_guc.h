// LikeOS-64 -- the GuC: firmware load, the command transport, submission.
#ifndef I915_GUC_H
#define I915_GUC_H

#include <kernel/ke/sched.h>

struct i915_device;
struct i915_engine;
struct i915_lrc;
struct drm_gem_object;

#define I915_GUC_MAX_CONTEXTS 4096 /* GuC context ids handed out here */

struct i915_guc {
	int wanted; /* the platform's policy asks for the firmware */
	int loaded; /* the microcontroller runs the image */
	int huc_loaded;
	int huc_authenticated;
	int comm; /* the command transport is registered and open */
	int submission; /* the engines submit through the GuC */
	uint32_t fw_version, huc_version;
	uint32_t wopcm_base, wopcm_size; /* the GuC's share of the WOPCM */
	/* the images in the GGTT, whole (header, code, signature) */
	struct drm_gem_object *guc_obj, *huc_obj, *huc_rsa_obj;
	uint32_t guc_ggtt, huc_ggtt, huc_rsa_ggtt;
	uint32_t guc_ucode_size, huc_ucode_size, guc_rsa_off, huc_rsa_off;
	/* the additional data structures and the golden contexts */
	struct drm_gem_object *ads_obj;
	uint32_t ads_ggtt;
	uint8_t *ads_vaddr;
	/* the command transport: two rings with a descriptor each */
	struct drm_gem_object *ct_obj;
	uint32_t ct_ggtt;
	uint8_t *ct_vaddr;
	spinlock_t ct_lock;
	uint16_t ct_fence;
	uint32_t ct_h2g_size, ct_g2h_size; /* dwords */
	/* context ids: 0 free, 1 in use, 2 waiting for the GuC to let go */
	uint8_t ctx_state[I915_GUC_MAX_CONTEXTS];
	uint32_t ctx_next;
	uint32_t g2h_events, g2h_unknown, resets_seen;
	uint32_t send_failures;
};

int i915_guc_init(struct i915_device *i915); /* load (late init) */
void i915_guc_fini(struct i915_device *i915);
int i915_guc_submission_enable(struct i915_device *i915);
void i915_guc_irq(struct i915_device *i915); /* the GuC-to-host interrupt */
void i915_guc_ct_process(struct i915_device *i915); /* the GuC's messages */
void i915_guc_submit(struct i915_engine *e); /* caller holds e->lock */
void i915_guc_lrc_release(struct i915_device *i915, struct i915_lrc *lrc);
void i915_guc_ggtt_invalidate(struct i915_device *i915);
int i915_guc_wants_load(struct i915_device *i915);
void i915_guc_irq_enable(struct i915_device *i915);

#endif
