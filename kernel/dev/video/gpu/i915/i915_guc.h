// LikeOS -- the GuC: firmware load, the command transport, submission.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef I915_GUC_H
#define I915_GUC_H

#include <kernel/ke/sched.h>

struct i915_device;
struct i915_engine;
struct i915_lrc;
struct i915_vm;
struct drm_gem_object;
struct i915_guc_ctx; /* a context id's bookkeeping (i915_guc.c) */

#define I915_GUC_MAX_CONTEXTS 4096 /* context ids handed out, per GuC */

/* One GuC: the primary GT's (i915->guc) or the standalone media GT's
 * (i915->media_guc, Meteor Lake).  `submission' on i915->guc says
 * whether the engines submit through the GuC at all; the media GuC's
 * copy follows it. */
struct i915_guc {
	int wanted; /* the platform's policy asks for the firmware */
	int loaded; /* the microcontroller runs the image */
	int huc_loaded;
	int huc_authenticated;
	int comm; /* the command transport is registered and open */
	int submission; /* the engines submit through the GuC */
	int slpc; /* the GuC runs the GT's frequency (SLPC) */
	/* which GuC this is: the media GT's, its register offset (applied
	 * to the GT's registers below 0x40000), and the registers the
	 * message interface goes through */
	int media;
	uint32_t gsi;
	uint32_t send_base, send_count, notify_reg;
	/* the image's versions: the firmware's and that of the submission
	 * interface it speaks (major << 16 | minor << 8 | patch) */
	uint32_t fw_version, submit_version, huc_version;
	uint32_t wopcm_base, wopcm_size; /* the GuC's share of the WOPCM */
	/* the images in the GGTT, whole (header, code, signature), and the
	 * signatures where the boot ROM reads them from memory */
	struct drm_gem_object *guc_obj, *guc_rsa_obj, *huc_obj, *huc_rsa_obj;
	uint32_t guc_ggtt, guc_rsa_ggtt, huc_ggtt, huc_rsa_ggtt;
	uint32_t guc_ucode_size, guc_rsa_size, huc_ucode_size, huc_rsa_size;
	uint32_t private_data_size;
	/* the firmware's log and error capture buffer */
	struct drm_gem_object *log_obj;
	uint32_t log_ggtt;
	/* the engines of this GuC's GT (I915_ENGINE_* bits), from the
	 * fuses, and the video engines with a scaler */
	uint32_t engine_mask;
	uint8_t vdbox_sfc_access;
	/* the additional data structures (ADS) */
	struct drm_gem_object *ads_obj;
	uint32_t ads_ggtt, ads_size;
	uint8_t *ads_vaddr;
	uint32_t ads_regset_off, ads_regset_size;
	uint32_t ads_golden_off, ads_golden_size;
	uint32_t ads_waklv_off, ads_capture_off, ads_private_off;
	uint32_t golden_off[5], golden_bytes[5]; /* per uAPI engine class, 0 = none */
	int golden_filled;
	void *regset; /* the save/restore lists, built once */
	uint16_t regset_count[32]; /* per engine id */
	/* the command transport: two descriptors, then the two rings */
	struct drm_gem_object *ct_obj;
	uint32_t ct_ggtt;
	uint8_t *ct_vaddr;
	spinlock_t ct_lock; /* the host-to-GuC ring and the context ids */
	spinlock_t g2h_lock; /* the GuC-to-host ring */
	uint32_t ct_h2g_size, ct_g2h_size; /* dwords */
	uint32_t ct_h2g_tail, ct_g2h_head;
	uint16_t ct_fence;
	int ct_broken;
	/* the one request at a time that waits for its answer */
	volatile uint32_t wait_fence;
	volatile int wait_done;
	volatile uint32_t wait_hxg;
	uint32_t wait_data[4];
	/* SLPC: the shared data, the range asked for */
	struct drm_gem_object *slpc_obj;
	uint32_t slpc_ggtt;
	uint8_t *slpc_vaddr;
	uint32_t rp0_mhz, rpe_mhz, rpn_mhz;
	/* context ids: their state, and the logical ring each belongs to */
	struct i915_guc_ctx *ctx;
	uint32_t ctx_next;
	int ctx_pending; /* ids with a message still to send */
	uint32_t g2h_events, g2h_unknown, resets_seen;
	uint32_t send_failures;
	/* what has been said once (the first registration, the first
	 * deregistration confirmed, transport trouble) */
	int reg_said, rel_said, dereg_said, ct_full_said;
	/* Xe2 on: the sequence number of the last translation invalidation
	 * asked of the GuC */
	uint32_t tlb_seqno;
};

/* Load the GuC firmware (late init): the primary GuC and, on a part
 * with a standalone media GT that submits through the GuC, the media
 * GuC as well.  0 when the primary GuC runs; whether the media GuC does
 * is i915->media_guc.loaded. */
int i915_guc_init(struct i915_device *i915);
void i915_guc_fini(struct i915_device *i915);
/* Submission through the GuC from now on (the engines are up): sets
 * i915->guc.submission once every loaded GuC accepted. */
int i915_guc_submission_enable(struct i915_device *i915);
/* After i915_gt_golden_init: the engines' recorded default state into
 * each GuC's golden context area. */
void i915_guc_ads_update_golden(struct i915_device *i915);
void i915_guc_irq(struct i915_device *i915); /* the GuC-to-host interrupt */
void i915_guc_ct_process(struct i915_device *i915); /* the GuCs' messages */
void i915_guc_submit(struct i915_engine *e); /* caller holds e->lock */
/* A logical ring going away: its scheduling switched off and its
 * registration dropped; the image, the ring and the address space
 * (`vm', may be NULL) are kept (referenced) until the GuC confirms. */
void i915_guc_lrc_release(struct i915_device *i915, struct i915_lrc *lrc, struct i915_vm *vm);
/* Switch scheduling of a registered logical ring off and wait (200 ms
 * at most, polling) until the GuC says the engine has let go of it: its
 * image is saved.  The next submission switches it on again.  0 or
 * -ETIMEDOUT. */
int i915_guc_lrc_quiesce(struct i915_device *i915, struct i915_lrc *lrc);
/* An engine that made no progress: the context it runs is made to be
 * preempted, which the GuC answers with a reset of that context when it
 * does not yield.  0 when the request went out. */
int i915_guc_engine_hang(struct i915_engine *e);
void i915_guc_ggtt_invalidate(struct i915_device *i915);
int i915_guc_wants_load(struct i915_device *i915);
/* The GuC's hardware configuration table (i915->hwconfig) let go of. */
void i915_guc_hwconfig_free(struct i915_device *i915);
void i915_guc_irq_enable(struct i915_device *i915);

#endif
