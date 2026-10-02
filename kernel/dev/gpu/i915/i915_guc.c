// LikeOS -- the GuC: firmware load, the command transport, submission.
//
// The GuC is a microcontroller in the GT.  Loaded with its firmware it
// can own command submission (the host registers contexts and asks for
// them to be scheduled; the GuC writes the execution lists), run the
// GT's frequency (SLPC), and authenticate the HuC, the media controller.
// Where a part's design makes GuC submission the only kind (DG2, Meteor
// Lake and Arrow Lake) this file is the whole path.  On Ice Lake and on
// the Gen12 parts from Alder Lake on (and DG1) the firmware is loaded for
// the HuC's sake, submission left with the execution lists; Tiger Lake
// and Rocket Lake, like Gen9, run without it.
//
// Meteor Lake has two GTs: the primary one (render, copy, compute) and a
// standalone media GT (video decode and enhancement) whose registers
// below 0x40000 sit 0x380000 higher; so have Lunar Lake, Battlemage,
// Panther Lake, Wildcat Lake and Nova Lake, whose GuCs take the same
// interfaces with the workarounds, the WOPCM sizes and the save/restore
// lists of Xe2 and Xe3.  Each has a GuC of its own, loaded
// with the same image; each has its own WOPCM partition, its own message
// registers and doorbell, its own command transport, context ids and
// golden contexts.  Everything below works on a struct i915_guc and the
// GT offset it carries.
//
// The interfaces are those of firmware major version 70: the CSS header
// of the image, the load parameters, the additional data structures
// (ADS), the HXG messages over MMIO and over the command transport
// buffers (CTB), and the version-1 submission interface (single logical
// ring contexts, no work queue).
//
// Nothing here has run on hardware.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2014-2025 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/i915_guc_reg.h>
#include <kernel/dev/gpu/i915/i915_xe2_reg.h>
#include <kernel/dev/gpu/i915/i915_wa_reg.h>
#include <kernel/dev/gpu/i915/i915_gt.h>
#include <kernel/dev/gpu/i915/i915_lrc_layout.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/firmware.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/uapi/drm/i915_drm.h>

#define GUC_VER(maj, min, pat) (((uint32_t)(maj) << 16) | ((uint32_t)(min) << 8) | (uint32_t)(pat))
#define GUC_VER_MAJOR(v) (((v) >> 16) & 0xff)
#define GUC_VER_MINOR(v) (((v) >> 8) & 0xff)
#define GUC_VER_PATCH(v) ((v) & 0xff)

/* ---- the messages ---------------------------------------------------------------- */

/* Every message starts with an HXG dword: who sent it, what kind it is,
 * and for a request or an event the action.  A response carries 28 bits
 * of data in the same dword; a failure an error and a hint. */
#define HXG_ORIGIN_GUC (1u << 31)
#define HXG_TYPE_SHIFT 28
#define HXG_TYPE(x) (((x) >> HXG_TYPE_SHIFT) & 7u)
#define HXG_TYPE_REQUEST 0u
#define HXG_TYPE_EVENT 1u
#define HXG_TYPE_FAST_REQUEST 2u
#define HXG_TYPE_NO_RESPONSE_BUSY 3u
#define HXG_TYPE_NO_RESPONSE_RETRY 5u
#define HXG_TYPE_RESPONSE_FAILURE 6u
#define HXG_TYPE_RESPONSE_SUCCESS 7u
#define HXG_REQUEST_ACTION_MASK 0x0fffffffu /* the action and its data0 */
#define HXG_EVENT_ACTION(x) ((x) & 0xffffu)
#define HXG_RESPONSE_DATA0(x) ((x) & 0x0fffffffu)
#define HXG_FAILURE_HINT(x) (((x) >> 16) & 0xfffu)
#define HXG_FAILURE_ERROR(x) ((x) & 0xffffu)
#define HXG_RETRY_REASON(x) ((x) & 0x0fffffffu)

#define GUC_ACTION_HOST2GUC_SELF_CFG 0x0508
#define GUC_ACTION_UPDATE_SCHEDULING_POLICIES_KLV 0x0509
#define GUC_ACTION_SCHED_CONTEXT 0x1000
#define GUC_ACTION_SCHED_CONTEXT_MODE_SET 0x1001
#define GUC_ACTION_SCHED_CONTEXT_MODE_DONE 0x1002
#define GUC_ACTION_CONTEXT_RESET_NOTIFICATION 0x1008
#define GUC_ACTION_ENGINE_FAILURE_NOTIFICATION 0x1009
#define GUC_ACTION_HOST2GUC_UPDATE_CONTEXT_POLICIES 0x100b
#define GUC_ACTION_HOST2GUC_PC_SLPC_REQUEST 0x3003
#define GUC_ACTION_AUTHENTICATE_HUC 0x4000
#define GUC_ACTION_REGISTER_CONTEXT 0x4502
#define GUC_ACTION_DEREGISTER_CONTEXT 0x4503
#define GUC_ACTION_HOST2GUC_CONTROL_CTB 0x4509
#define GUC_ACTION_DEREGISTER_CONTEXT_DONE 0x4600
#define GUC_ACTION_SET_ENG_UTIL_BUFF 0x550a
#define GUC_ACTION_GET_HWCONFIG 0x4100
#define GUC_ACTION_TLB_INVALIDATION 0x7000
#define GUC_ACTION_TLB_INVALIDATION_DONE 0x7001
#define GUC_TLB_INVAL_TYPE_GUC 0x3u
#define GUC_TLB_INVAL_MODE_HEAVY (0x0u << 8)
#define GUC_TLB_INVAL_FLUSH_CACHE (1u << 31)
#define GUC_ACTION_STATE_CAPTURE_NOTIFICATION 0x8002
#define GUC_ACTION_NOTIFY_FLUSH_LOG_BUFFER_TO_FILE 0x8003
#define GUC_ACTION_NOTIFY_CRASH_DUMP_POSTED 0x8004
#define GUC_ACTION_NOTIFY_EXCEPTION 0x8005

/* Key-length-value entries: a dword with the key and the number of
 * value dwords, then the value. */
#define KLV(key, len) (((uint32_t)(key) << 16) | (uint32_t)(len))
#define KLV_SELF_CFG_H2G_CTB_ADDR 0x0902
#define KLV_SELF_CFG_H2G_CTB_DESC_ADDR 0x0903
#define KLV_SELF_CFG_H2G_CTB_SIZE 0x0904
#define KLV_SELF_CFG_G2H_CTB_ADDR 0x0905
#define KLV_SELF_CFG_G2H_CTB_DESC_ADDR 0x0906
#define KLV_SELF_CFG_G2H_CTB_SIZE 0x0907
#define KLV_SCHED_RENDER_COMPUTE_YIELD 0x1001
#define KLV_CTX_EXECUTION_QUANTUM 0x2001
#define KLV_CTX_PREEMPTION_TIMEOUT 0x2002
#define KLV_CTX_SCHEDULING_PRIORITY 0x2003
#define KLV_CTX_PREEMPT_TO_IDLE_ON_QUANTUM_EXPIRY 0x2004
#define KLV_WA_SERIALIZED_RA_MODE 0x9001
#define KLV_WA_BLOCK_INTERRUPTS_WHEN_MGSR_BLOCKED 0x9002
#define KLV_WA_GAM_PFQ_SHADOW_TAIL_POLLING 0x9005
#define KLV_WA_AVOID_GFX_CLEAR_WHILE_ACTIVE 0x9006
#define KLV_WA_DISABLE_MTP_DURING_ASYNC_COMPUTE 0x9007
#define KLV_WA_NP_RD_WRITE_TO_CLEAR_RCSM_AT_CGP_LATE_RESTORE 0x9008
#define KLV_WA_BACK_TO_BACK_RCS_ENGINE_RESET 0x9009
#define KLV_WA_WAKE_POWER_DOMAINS_FOR_OUTBOUND_MMIO 0x900a
#define KLV_WA_RESET_BB_STACK_PTR_ON_VF_SWITCH 0x900b
#define KLV_WA_RESTORE_UNSAVED_MEDIA_CONTROL_REG 0x900c
#define KLV_WA_CLR_CS_INDIRECT_RING_STATE_IF_IDLE_AT_CTX_REG 0x900e
#define KLV_WA_REMAP_RANGED_TLB_INV 0x900f
#define KLV_WA_IGNORE_MMIO_READ_SEM_TOKEN_64 0x9010
/* the keys of the hardware configuration table the steering reads */
#define HWCONFIG_ATTR_MAX_SLICES 1
#define HWCONFIG_ATTR_MAX_SUBSLICES 70

#define GUC_CTB_CONTROL_ENABLE 1u
#define GUC_CONTEXT_DISABLE 0u
#define GUC_CONTEXT_ENABLE 1u
#define GUC_CLIENT_PRIORITY_KMD_NORMAL 2u
#define CONTEXT_REGISTRATION_FLAG_KMD (1u << 0)
#define GEN12_CTX_PRIORITY_NORMAL (1u << 9) /* the descriptor's EU priority */

/* The render/compute yield: how long, and what share of it, the GuC
 * lets one of the two classes wait for the other. */
#define GLOBAL_SCHEDULE_POLICY_RC_YIELD_DURATION 100 /* ms */
#define GLOBAL_SCHEDULE_POLICY_RC_YIELD_RATIO 50 /* percent */
#define GLOBAL_POLICY_DEFAULT_DPC_PROMOTE_TIME_US 500000
#define GLOBAL_POLICY_MAX_NUM_WI 15

/* ---- the load parameters ----------------------------------------------------------- */

#define GUC_CTL_LOG_PARAMS 0
#define GUC_LOG_VALID (1u << 0)
#define GUC_LOG_NOTIFY_ON_HALF_FULL (1u << 1)
#define GUC_LOG_CAPTURE_ALLOC_UNITS (1u << 2)
#define GUC_LOG_LOG_ALLOC_UNITS (1u << 3)
#define GUC_LOG_CRASH_SHIFT 4
#define GUC_LOG_DEBUG_SHIFT 6
#define GUC_LOG_CAPTURE_SHIFT 10
#define GUC_LOG_BUF_ADDR_SHIFT 12
#define GUC_CTL_WA 1
#define GUC_WA_GAM_CREDITS (1u << 10)
#define GUC_WA_DUAL_QUEUE (1u << 11)
#define GUC_WA_RCS_RESET_BEFORE_RC6 (1u << 13)
#define GUC_WA_PRE_PARSER (1u << 14)
#define GUC_WA_CONTEXT_ISOLATION (1u << 15)
#define GUC_WA_RCS_CCS_SWITCHOUT (1u << 16)
#define GUC_WA_HOLD_CCS_SWITCHOUT (1u << 17)
#define GUC_WA_POLLCS (1u << 18)
#define GUC_WA_ENABLE_TSC_CHECK_ON_RC6 (1u << 22)
#define GUC_CTL_FEATURE 2
#define GUC_CTL_ENABLE_SLPC (1u << 2)
#define GUC_CTL_ENABLE_LITE_RESTORE (1u << 4)
#define GUC_CTL_MAIN_GAMCTRL_QUEUES (1u << 9)
#define GUC_CTL_DISABLE_SCHEDULER (1u << 14)
#define GUC_CTL_ENABLE_L2FLUSH_OPT (1u << 15)
#define GUC_CTL_DEBUG 3
#define GUC_LOG_DISABLED (1u << 6)
#define GUC_CTL_ADS 4
#define GUC_ADS_ADDR_SHIFT 1
#define GUC_CTL_DEVID 5
#define GUC_CTL_MAX_DWORDS (SOFT_SCRATCH_COUNT - 2) /* [1..14] */

/* The log: crash dumps and the debug log in 4 KB units, the error
 * capture in 1 MB units, after a page of buffer states. */
#define GUC_LOG_CRASH_BYTES (8u * 1024)
#define GUC_LOG_DEBUG_BYTES (64u * 1024)
#define GUC_LOG_CAPTURE_BYTES (1024u * 1024)
#define GUC_LOG_BYTES (4096u + GUC_LOG_DEBUG_BYTES + GUC_LOG_CRASH_BYTES + GUC_LOG_CAPTURE_BYTES)

/* The microkernel's progress (GUC_STATUS bits 15:8) and the boot
 * ROM's (bits 7:1). */
#define GUC_LOAD_STATUS_ERROR_DEVID_BUILD_MISMATCH 0x02
#define GUC_LOAD_STATUS_GUC_PREPROD_BUILD_MISMATCH 0x03
#define GUC_LOAD_STATUS_ERROR_DEVID_INVALID_GUCTYPE 0x04
#define GUC_LOAD_STATUS_HWCONFIG_START 0x05
#define GUC_LOAD_STATUS_HWCONFIG_ERROR 0x07
#define GUC_LOAD_STATUS_DPC_ERROR 0x60
#define GUC_LOAD_STATUS_EXCEPTION 0x70
#define GUC_LOAD_STATUS_INIT_DATA_INVALID 0x71
#define GUC_LOAD_STATUS_MPU_DATA_INVALID 0x73
#define GUC_LOAD_STATUS_INIT_MMIO_SAVE_RESTORE_INVALID 0x74
#define GUC_LOAD_STATUS_KLV_WORKAROUND_INIT_ERROR 0x75
#define GUC_LOAD_STATUS_RUNNING 0xf0
#define BOOTROM_STATUS_NO_KEY_FOUND 0x13
#define BOOTROM_STATUS_PROD_KEY_CHECK_FAILURE 0x2b
#define BOOTROM_STATUS_RSA_FAILED 0x50
#define BOOTROM_STATUS_PAVPC_FAILED 0x73
#define BOOTROM_STATUS_WOPCM_FAILED 0x74
#define BOOTROM_STATUS_LOADLOC_FAILED 0x75
#define BOOTROM_STATUS_JUMP_FAILED 0x77
#define BOOTROM_STATUS_RC6CTXCONFIG_FAILED 0x79
#define BOOTROM_STATUS_MPUMAP_INCORRECT 0x7a
#define BOOTROM_STATUS_EXCEPTION 0x7e

/* ---- the additional data structures -------------------------------------------------- */

#define GUC_MAX_ENGINE_CLASSES 16
#define GUC_MAX_INSTANCES_PER_CLASS 32
#define GUC_CAPTURE_LIST_INDEX_MAX 2
#define GUC_GENERIC_GT_SYSINFO_SLICE_ENABLED 0
#define GUC_GENERIC_GT_SYSINFO_VDBOX_SFC_SUPPORT_MASK 1
#define GUC_GENERIC_GT_SYSINFO_DOORBELL_COUNT_PER_SQIDI 2
#define GUC_GENERIC_GT_SYSINFO_MAX 16
#define GUC_RENDER_CLASS 0
#define GUC_VIDEO_CLASS 1
#define GUC_VIDEOENHANCE_CLASS 2
#define GUC_BLITTER_CLASS 3
#define GUC_COMPUTE_CLASS 4

struct guc_mmio_reg {
	uint32_t offset;
	uint32_t value;
	uint32_t flags;
	uint32_t mask;
} __attribute__((packed));
#define GUC_REGSET_MASKED (1u << 0)
#define GUC_REGSET_NEEDS_STEERING (1u << 1)
#define GUC_REGSET_STEERING(group, instance)                                              \
	((((uint32_t)(group) & 0xf) << 12) | (((uint32_t)(instance) & 0xf) << 20) |      \
	 GUC_REGSET_NEEDS_STEERING)

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
	uint32_t dpc_promote_time; /* microseconds */
	uint32_t is_valid; /* set: the values are to be taken */
	uint32_t max_num_work_items;
	uint32_t global_flags;
	uint32_t reserved[4];
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

/* The fixed part of the ADS object; the register lists follow it, then
 * (each page-aligned) the golden contexts, the workaround KLVs, the
 * capture lists and the firmware's private data. */
struct guc_ads_blob {
	struct guc_ads ads;
	struct guc_policies policies;
	struct guc_gt_system_info system_info;
	struct guc_engine_usage_record engine_usage[GUC_MAX_ENGINE_CLASSES][GUC_MAX_INSTANCES_PER_CLASS];
} __attribute__((packed));

/* What precedes the engine state in a context image: the per-process
 * status page and the execution-list part of the register state. */
#define LR_HW_CONTEXT_BYTES (80 * 4)
#define XEHP_LR_HW_CONTEXT_BYTES (96 * 4)

/* ---- SLPC ----------------------------------------------------------------------------- */

#define SLPC_EVENT(id, argc) (((uint32_t)(id) << 8) | (uint32_t)(argc))
#define SLPC_EVENT_RESET 0
#define SLPC_EVENT_QUERY_TASK_STATE 5
#define SLPC_EVENT_PARAMETER_SET 6
#define SLPC_PARAM_TASK_ENABLE_GTPERF 0
#define SLPC_PARAM_TASK_DISABLE_GTPERF 1
#define SLPC_PARAM_TASK_ENABLE_BALANCER 2
#define SLPC_PARAM_TASK_DISABLE_BALANCER 3
#define SLPC_PARAM_TASK_ENABLE_DCC 4
#define SLPC_PARAM_TASK_DISABLE_DCC 5
#define SLPC_PARAM_GLOBAL_MIN_GT_UNSLICE_FREQ_MHZ 6
#define SLPC_PARAM_GLOBAL_MAX_GT_UNSLICE_FREQ_MHZ 7
#define SLPC_PARAM_MEDIA_FF_RATIO_MODE 24
#define SLPC_PARAM_STRATEGIES 26
#define SLPC_PARAM_POWER_PROFILE 27
#define SLPC_PARAM_IGNORE_EFFICIENT_FREQUENCY 28
#define SLPC_GLOBAL_STATE_RUNNING 3
#define SLPC_MEDIA_RATIO_MODE_DYNAMIC_CONTROL 0
#define SLPC_OPTIMIZED_STRATEGY_COMPUTE (1u << 0)
#define SLPC_POWER_PROFILES_BASE 0
#define SLPC_MAX_OVERRIDE_PARAMETERS 256
#define SLPC_SHARED_DATA_BYTES 8192

/* The shared data: a header line, the platform information line, the
 * task state line, then the override parameters (a bit per parameter
 * that is overridden, and the values), all in the first page. */
struct slpc_shared_data_head {
	uint32_t size;
	uint32_t global_state;
	uint32_t display_data_addr;
	uint8_t pad0[64 - 12];
	uint8_t platform_info[64];
	uint32_t task_status;
	uint32_t task_freq;
	uint8_t pad1[64 - 8];
	uint32_t override_bits[SLPC_MAX_OVERRIDE_PARAMETERS / 32];
	uint32_t override_values[SLPC_MAX_OVERRIDE_PARAMETERS];
} __attribute__((packed));

/* ---- context ids ------------------------------------------------------------------------- */

#define GUC_CTX_FREE 0
#define GUC_CTX_LIVE 1 /* the host's logical ring uses it */
#define GUC_CTX_DYING 2 /* let go of by the host; the GuC not yet done with it */

#define GUC_SEND_NONE 0
#define GUC_SEND_DISABLE 1 /* scheduling off, then the deregistration */
#define GUC_SEND_DEREGISTER 2

struct i915_guc_ctx {
	uint8_t state;
	uint8_t pending_enable, pending_disable;
	uint8_t destroy; /* deregister as soon as the GuC has it disabled */
	uint8_t need_send; /* GUC_SEND_*: did not fit the ring yet */
	uint8_t policy_required; /* the policies have still to go */
	struct i915_lrc *lrc; /* while LIVE */
	/* the image and the ring, referenced while DYING: the engine may
	 * still save the context into its image until the GuC lets go --
	 * and the address space its image names, whose tables the engine
	 * walks for as long as the context can still run */
	struct drm_gem_object *img, *ring;
	struct i915_vm *vm;
};

/* ---- small helpers ------------------------------------------------------------------------ */

static const char *guc_name(const struct i915_guc *g)
{
	return g->media ? "media GuC" : "GuC";
}

/* A register of this GuC's GT. */
static inline uint32_t guc_reg(const struct i915_guc *g, uint32_t reg)
{
	return i915_gt_reg(g->gsi, reg);
}

static uint32_t guc_read(struct i915_device *i915, const struct i915_guc *g, uint32_t reg)
{
	return i915_read32(i915, guc_reg(g, reg));
}

static void guc_write(struct i915_device *i915, const struct i915_guc *g, uint32_t reg, uint32_t val)
{
	i915_write32(i915, guc_reg(g, reg), val);
}

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static unsigned popcount32(uint32_t v)
{
	unsigned n = 0;
	for (; v; v &= v - 1)
		n++;
	return n;
}

static unsigned first_bit(uint32_t v)
{
	for (unsigned i = 0; i < 32; i++)
		if (v & (1u << i))
			return i;
	return 0;
}

/* Lines the processor wrote, out to memory; lines the GuC wrote, out of
 * the processor's caches.  Where the part shares its last-level cache
 * with the processor nothing is needed. */
static void clflush_range(const void *p, uint32_t len)
{
	uintptr_t a = (uintptr_t)p & ~(uintptr_t)63;
	uintptr_t end = (uintptr_t)p + len;
	for (; a < end; a += 64)
		__asm__ volatile("clflush (%0)" ::"r"(a) : "memory");
	__asm__ volatile("mfence" ::: "memory");
}

static void guc_flush(struct i915_device *i915, const void *p, uint32_t len)
{
	if (!(i915->info->flags & I915_INFO_HAS_LLC))
		clflush_range(p, len);
	else
		__asm__ volatile("mfence" ::: "memory");
}

static struct i915_guc *guc_of_engine(struct i915_device *i915, const struct i915_engine *e)
{
	return e->gsi_offset ? &i915->media_guc : &i915->guc;
}

/* Does this part submit through the GuC (the firmware is loaded with its
 * scheduler on, SLPC, the full ADS)? */
static int guc_wants_submission(const struct i915_device *i915)
{
	return !!(i915->info->flags & I915_INFO_GUC_MANDATORY);
}

static int guc_wants_slpc(const struct i915_device *i915)
{
	return guc_wants_submission(i915) && i915->info->gen >= 12;
}

/* An object the GuC reads and writes: physically contiguous, mapped by
 * the processor through the direct map, bound in the global space where
 * the GuC reaches it.  Wa_22016122933: the media GT of media version
 * 13.00 shares memory with the host uncached; its objects are bound so
 * and the processor's caches are flushed around every access. */
static struct drm_gem_object *guc_alloc(struct i915_device *i915, struct i915_guc *g, uint32_t size,
					 uint32_t *ggtt, uint8_t **vaddr)
{
	int uc = g->media && i915->media_ip == I915_IP(13, 0);
	struct drm_gem_object *o;

	size = (size + 4095) & ~4095u;
	o = i915_gem_alloc_bound(i915, size, uc, 1, ggtt);
	if (!o)
		return NULL;
	*vaddr = phys_to_virt(o->pages[0]);
	mm_memset(*vaddr, 0, size);
	clflush_range(*vaddr, size);
	return o;
}

/* ---- the firmware image ------------------------------------------------------------------- */

/* The CSS header: 128 bytes, then the code, then the signature (and a
 * modulus and exponent the driver does not need).  Sizes in dwords. */
#define UC_CSS_HEADER_BYTES 128
#define CSS_HEADER_SIZE_DW 1
#define CSS_SIZE_DW 6
#define CSS_KEY_SIZE_DW 7
#define CSS_MODULUS_SIZE_DW 8
#define CSS_EXPONENT_SIZE_DW 9
#define CSS_SW_VERSION 16
#define CSS_VF_VERSION 17
#define CSS_PRIVATE_DATA_SIZE 30

#define GEN11_WOPCM_SIZE (2u << 20)
#define GEN9_WOPCM_SIZE (1u << 20)
#define MAX_WOPCM_SIZE (8u << 20)
#define LNL_WOPCM_SIZE (8u << 20) /* Lunar Lake and every part after it */
#define NVLP_MAX_WOPCM_SIZE (16u << 20)

/* The WOPCM: 1 MB on Gen9, 2 MB from Gen11, 8 MB from Lunar Lake on. */
static uint32_t wopcm_size_of(const struct i915_device *i915)
{
	if (i915->gt_ip >= I915_IP(20, 0))
		return LNL_WOPCM_SIZE;
	return i915->info->gen >= 11 ? GEN11_WOPCM_SIZE : GEN9_WOPCM_SIZE;
}

struct uc_image {
	void *data; /* the file (firmware_request's) */
	size_t len;
	uint32_t ucode_size, rsa_size, version, vf_version, private_data_size;
};

static int uc_image_open(struct i915_device *i915, const char *what, const char *name,
			 struct uc_image *img)
{
	int rc = firmware_request(name, &img->data, &img->len);
	if (rc) {
		kprintf("[drm] i915: %s firmware %s not available (%d)\n", what, name, rc);
		img->data = NULL;
		return rc;
	}
	const uint8_t *css = img->data;
	if (img->len < UC_CSS_HEADER_BYTES) {
		kprintf("[drm] i915: %s firmware %s: too short (%u bytes)\n", what, name,
			(unsigned)img->len);
		goto bad;
	}
	/* the header's own size is what the whole header (CSS, key,
	 * modulus, exponent) leaves after the three keys */
	int64_t hdr_dw = rd32(css + CSS_HEADER_SIZE_DW * 4);
	int64_t key_dw = rd32(css + CSS_KEY_SIZE_DW * 4);
	int64_t mod_dw = rd32(css + CSS_MODULUS_SIZE_DW * 4);
	int64_t exp_dw = rd32(css + CSS_EXPONENT_SIZE_DW * 4);
	int64_t size_dw = rd32(css + CSS_SIZE_DW * 4);
	if ((hdr_dw - key_dw - mod_dw - exp_dw) * 4 != UC_CSS_HEADER_BYTES || size_dw < hdr_dw) {
		kprintf("[drm] i915: %s firmware %s: unexpected header size\n", what, name);
		goto bad;
	}
	img->ucode_size = (uint32_t)(size_dw - hdr_dw) * 4;
	img->rsa_size = (uint32_t)key_dw * 4;
	if (img->len < (size_t)UC_CSS_HEADER_BYTES + img->ucode_size + img->rsa_size) {
		kprintf("[drm] i915: %s firmware %s: truncated (%u < %u bytes)\n", what, name,
			(unsigned)img->len, UC_CSS_HEADER_BYTES + img->ucode_size + img->rsa_size);
		goto bad;
	}
	uint32_t wopcm = wopcm_size_of(i915);
	if (UC_CSS_HEADER_BYTES + img->ucode_size >= wopcm) {
		kprintf("[drm] i915: %s firmware %s: larger than the WOPCM\n", what, name);
		goto bad;
	}
	if (img->rsa_size > 4096 || !img->rsa_size) {
		kprintf("[drm] i915: %s firmware %s: unexpected signature size %u\n", what, name,
			img->rsa_size);
		goto bad;
	}
	img->version = rd32(css + CSS_SW_VERSION * 4) & 0xffffff;
	img->vf_version = rd32(css + CSS_VF_VERSION * 4) & 0xffffff;
	img->private_data_size = rd32(css + CSS_PRIVATE_DATA_SIZE * 4);
	return 0;
bad:
	firmware_release(img->data);
	img->data = NULL;
	return -EINVAL;
}

/* The submission interface the image speaks: in the header from 70.6
 * on; 1.1.0 from 70.3, 1.0.0 from 70.0, older firmware older interfaces. */
static uint32_t guc_submit_version(uint32_t fw, uint32_t vf)
{
	if (GUC_VER_MAJOR(fw) >= 70) {
		if (GUC_VER_MINOR(fw) >= 6)
			return vf;
		if (GUC_VER_MINOR(fw) >= 3)
			return GUC_VER(1, 1, 0);
		return GUC_VER(1, 0, 0);
	}
	if (GUC_VER_MAJOR(fw) >= 69)
		return GUC_VER(0, 10, 0);
	return GUC_VER(0, 1, 0);
}

/* A whole image into an object of the global space the DMA engine reads
 * from: uncached, page by page, and out of the processor's caches. */
static struct drm_gem_object *image_to_ggtt(struct i915_device *i915, const void *data, size_t len,
					    uint32_t *ggtt)
{
	uint64_t size = (len + 4095) & ~4095ULL;
	struct drm_gem_object *o = i915_gem_alloc_bound(i915, size, 1, 0, ggtt);
	if (!o)
		return NULL;
	for (uint32_t p = 0; p < o->npages; p++) {
		uint8_t *va = phys_to_virt(o->pages[p]);
		size_t at = (size_t)p * 4096;
		size_t n = len > at ? len - at : 0;
		if (n > 4096)
			n = 4096;
		if (n)
			mm_memcpy(va, (const uint8_t *)data + at, n);
		if (n < 4096)
			mm_memset(va + n, 0, 4096 - n);
		clflush_range(va, 4096);
	}
	return o;
}

/* ---- the engines of a GuC's GT ---------------------------------------------------------- */

#define VIDEO_ENGINES (I915_ENGINE_VCS0 | I915_ENGINE_VCS1 | I915_ENGINE_VCS2 | I915_ENGINE_VCS3 | \
		       I915_ENGINE_VECS0 | I915_ENGINE_VECS1)

struct guc_engine_desc {
	int id;
	uint8_t class, instance;
	uint32_t bit, base;
};

static const struct guc_engine_desc guc_engines[] = {
	{ I915_RCS0, I915_ENGINE_CLASS_RENDER, 0, I915_ENGINE_RCS0, RENDER_RING_BASE },
	{ I915_BCS0, I915_ENGINE_CLASS_COPY, 0, I915_ENGINE_BCS0, BLT_RING_BASE },
	{ I915_VCS0, I915_ENGINE_CLASS_VIDEO, 0, I915_ENGINE_VCS0, GEN11_BSD_RING_BASE },
	{ I915_VCS1, I915_ENGINE_CLASS_VIDEO, 1, I915_ENGINE_VCS1, GEN11_BSD2_RING_BASE },
	{ I915_VCS2, I915_ENGINE_CLASS_VIDEO, 2, I915_ENGINE_VCS2, GEN11_BSD3_RING_BASE },
	{ I915_VCS3, I915_ENGINE_CLASS_VIDEO, 3, I915_ENGINE_VCS3, GEN11_BSD4_RING_BASE },
	{ I915_VECS0, I915_ENGINE_CLASS_VIDEO_ENHANCE, 0, I915_ENGINE_VECS0, GEN11_VEBOX_RING_BASE },
	{ I915_VECS1, I915_ENGINE_CLASS_VIDEO_ENHANCE, 1, I915_ENGINE_VECS1, GEN11_VEBOX2_RING_BASE },
	{ I915_CCS0, I915_ENGINE_CLASS_COMPUTE, 0, I915_ENGINE_CCS0, GEN12_COMPUTE0_RING_BASE },
	{ I915_CCS1, I915_ENGINE_CLASS_COMPUTE, 1, I915_ENGINE_CCS1, GEN12_COMPUTE1_RING_BASE },
	{ I915_CCS2, I915_ENGINE_CLASS_COMPUTE, 2, I915_ENGINE_CCS2, GEN12_COMPUTE2_RING_BASE },
	{ I915_CCS3, I915_ENGINE_CLASS_COMPUTE, 3, I915_ENGINE_CCS3, GEN12_COMPUTE3_RING_BASE },
};
#define GUC_NUM_ENGINE_DESCS (sizeof(guc_engines) / sizeof(guc_engines[0]))

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

/* Which engines the fuses leave, before the engines themselves are set
 * up (the GuC is loaded first): the video engines the media fuses
 * enable, the compute engines whose quarter of the dual subslices is
 * there.  Also which video engines have a scaler of their own. */
static uint32_t guc_fused_engines(struct i915_device *i915, uint8_t *sfc_access)
{
	uint32_t mask = i915->info->engine_mask;
	uint32_t gsi = i915->has_media_gt ? I915_MEDIA_GT_BASE : 0;

	*sfc_access = 0;
	if (i915->media_ip >= I915_IP(11, 0)) {
		uint32_t fuse = i915_read32(i915, gsi + GEN11_GT_VEBOX_VDBOX_DISABLE);
		uint32_t sfc = 0xff;
		unsigned logical = 0;

		/* the register says what is disabled; from Xe_HP media on,
		 * what is enabled */
		if (i915->media_ip < I915_IP(12, 55))
			fuse = ~fuse;
		uint32_t vdbox = fuse & GEN11_GT_VDBOX_DISABLE_MASK;
		uint32_t vebox = (fuse & GEN11_GT_VEBOX_DISABLE_MASK) >> GEN11_GT_VEBOX_DISABLE_SHIFT;
		if (i915->media_ip >= I915_IP(12, 55))
			sfc = XEHP_SFC_ENABLE(i915_read32(i915, gsi + HSW_PAVP_FUSE1));
		for (unsigned i = 0; i < 4; i++) {
			uint32_t bit = I915_ENGINE_VCS0 << i;
			if (!(mask & bit)) {
				vdbox &= ~(1u << i);
				continue;
			}
			if (!(vdbox & (1u << i))) {
				mask &= ~bit;
				continue;
			}
			/* Gen11: a scaler with every other engine counted
			 * among those present; Gen12: with every even one,
			 * and an odd one whose even neighbour is fused off */
			int has = 0;
			if (sfc & (1u << (i / 2))) {
				if (i915->media_ip >= I915_IP(12, 0))
					has = (i % 2 == 0) || !(vdbox & (1u << (i - 1)));
				else
					has = logical % 2 == 0;
			}
			if (has)
				*sfc_access |= (uint8_t)(1u << i);
			logical++;
		}
		for (unsigned i = 0; i < 2; i++) {
			uint32_t bit = I915_ENGINE_VECS0 << i;
			if ((mask & bit) && !(vebox & (1u << i)))
				mask &= ~bit;
		}
	}
	uint32_t ccs = (mask >> 8) & 0xf;
	/* Xe2 on: the compute engines the fuse register names
	 * (i915_xe2_compute_fuses); the GuC is told of all of them even
	 * where only the first is driven */
	if (i915->gt_ip >= I915_IP(20, 0))
		return (mask & ~(0xfu << 8)) | ((ccs & i915->ccs_fused) << 8);
	if (i915->gt_ip >= I915_IP(12, 50) && popcount32(ccs) > 1) {
		for (unsigned i = 0; i < 4; i++) {
			uint32_t quarter = (i915->dss_compute >> (i * 8)) & 0xff;
			if ((ccs & (1u << i)) && !quarter)
				mask &= ~(I915_ENGINE_CCS0 << i);
		}
	}
	return mask;
}

/* The instances of a class on this GuC's GT, as a mask of physical
 * instances. */
static uint32_t guc_class_mask(const struct i915_guc *g, int uapi_class)
{
	uint32_t m = 0;
	for (unsigned i = 0; i < GUC_NUM_ENGINE_DESCS; i++)
		if (guc_engines[i].class == uapi_class && (g->engine_mask & guc_engines[i].bit))
			m |= 1u << guc_engines[i].instance;
	return m;
}

/* The logical instance the GuC knows an engine by: the present
 * instances of the class numbered in order -- for the video engines in
 * the order 0, 2, 4, 6, 1, 3, 5, 7, which keeps the split-frame pairs
 * apart. */
static unsigned guc_logical_instance(struct i915_device *i915, const struct i915_guc *g,
				     int uapi_class, unsigned instance)
{
	static const uint8_t vcs_order[8] = { 0, 2, 4, 6, 1, 3, 5, 7 };
	uint32_t present = guc_class_mask(g, uapi_class);
	unsigned logical = 0;

	for (unsigned j = 0; j < 8; j++) {
		unsigned phys = (uapi_class == I915_ENGINE_CLASS_VIDEO &&
				 i915->media_ip >= I915_IP(11, 0)) ? vcs_order[j] : j;
		if (!(present & (1u << phys)))
			continue;
		if (phys == instance)
			return logical;
		logical++;
	}
	return instance;
}

/* ---- the WOPCM partition ------------------------------------------------------------------- */

#define WOPCM_RESERVED_SIZE (16u * 1024) /* above the HuC's image */
#define GUC_WOPCM_RESERVED (16u * 1024) /* at the start of the GuC's share */
#define GUC_WOPCM_STACK_RESERVED (8u * 1024)
#define GUC_WOPCM_OFFSET_ALIGNMENT (1u << GUC_WOPCM_OFFSET_SHIFT)
#define BXT_WOPCM_RC6_CTX_RESERVED (24u * 1024)
#define ICL_WOPCM_HW_CTX_RESERVED (36u * 1024)
#define GEN9_GUC_FW_RESERVED (128u * 1024)
#define GEN9_GUC_WOPCM_OFFSET (GUC_WOPCM_RESERVED + GEN9_GUC_FW_RESERVED)

/* The parts whose platform firmware programs (and locks) the GuC's own
 * registers, the WOPCM partition among them.  From Xe2 on the host
 * programs the partition itself where the firmware left it open. */
static int guc_deprivileged(const struct i915_device *i915)
{
	return i915->gt_ip >= I915_IP(12, 55) && i915->gt_ip < I915_IP(20, 0);
}

static int wopcm_init(struct i915_device *i915, struct i915_guc *g, uint32_t guc_fw_size,
		      uint32_t huc_fw_size)
{
	int gen9 = i915->info->gen == 9;
	int xe2 = i915->gt_ip >= I915_IP(20, 0);
	uint32_t wopcm = wopcm_size_of(i915);
	uint32_t ctx_rsvd = (gen9 && (i915->info->flags & I915_INFO_IS_LP)) ?
				    BXT_WOPCM_RC6_CTX_RESERVED :
			    i915->info->gen >= 11 ? ICL_WOPCM_HW_CTX_RESERVED :
						    0;
	uint32_t size_reg = guc_read(i915, g, GUC_WOPCM_SIZE);
	uint32_t off_reg = guc_read(i915, g, DMA_GUC_WOPCM_OFFSET);
	uint32_t base, size;
	int locked = 0;

	if ((size_reg & GUC_WOPCM_SIZE_LOCKED) && (off_reg & GUC_WOPCM_OFFSET_VALID)) {
		base = off_reg & GUC_WOPCM_OFFSET_MASK;
		size = size_reg & GUC_WOPCM_SIZE_MASK;
		locked = 1;
		i915_dbg("[drm] i915: %s WOPCM partitioned already: %u KB at %u KB\n", guc_name(g),
			 size >> 10, base >> 10);
		/* Programmed by the platform firmware where the host may not:
		 * its sizes are trusted, the whole WOPCM taken as the most
		 * there can be. */
		if (guc_deprivileged(i915) &&
		    !(guc_read(i915, g, GUC_SHIM_CONTROL2) & GUC_IS_PRIVILEGED))
			wopcm = MAX_WOPCM_SIZE;
		/* Xe2 on: checked against the most WOPCM there can be */
		if (xe2)
			wopcm = i915->info->platform == I915_PLATFORM_NOVALAKE_P ?
					NVLP_MAX_WOPCM_SIZE : MAX_WOPCM_SIZE;
	} else {
		/* A media GT comes with the GuC deprivileged: its partition
		 * (which shares the WOPCM with the primary GT and the GSC)
		 * is the platform firmware's to set -- before Xe2, from
		 * which on the host lays it out itself. */
		if (!xe2 && (g->media || guc_deprivileged(i915))) {
			kprintf("[drm] i915: %s WOPCM partition not programmed by the platform firmware\n",
				guc_name(g));
			return -EIO;
		}
		base = (huc_fw_size + WOPCM_RESERVED_SIZE + GUC_WOPCM_OFFSET_ALIGNMENT - 1) &
		       ~(GUC_WOPCM_OFFSET_ALIGNMENT - 1);
		if (base > wopcm - ctx_rsvd)
			base = wopcm - ctx_rsvd;
		size = (wopcm - ctx_rsvd - base) & GUC_WOPCM_SIZE_MASK;
	}
	/* the layout */
	if (base + size > wopcm - ctx_rsvd || base + size < base) {
		kprintf("[drm] i915: %s WOPCM layout invalid: %u KB + %u KB > %u KB\n", guc_name(g),
			base >> 10, size >> 10, (wopcm - ctx_rsvd) >> 10);
		return -E2BIG;
	}
	if (size < guc_fw_size + GUC_WOPCM_RESERVED + GUC_WOPCM_STACK_RESERVED) {
		kprintf("[drm] i915: %s WOPCM share too small for the firmware: %u KB < %u KB\n",
			guc_name(g), size >> 10,
			(guc_fw_size + GUC_WOPCM_RESERVED + GUC_WOPCM_STACK_RESERVED) >> 10);
		return -E2BIG;
	}
	if (huc_fw_size && base < huc_fw_size + WOPCM_RESERVED_SIZE) {
		kprintf("[drm] i915: no room in the WOPCM for the HuC: %u KB < %u KB\n", base >> 10,
			(huc_fw_size + WOPCM_RESERVED_SIZE) >> 10);
		return -E2BIG;
	}
	if (gen9) {
		/* the share at least a dword beyond the firmware's reserved
		 * offset, and room for the HuC in it */
		uint32_t off = base + GEN9_GUC_WOPCM_OFFSET;
		if (off > size || size - off < 4) {
			kprintf("[drm] i915: WOPCM layout fails the Gen9 dword-gap rule\n");
			return -E2BIG;
		}
		if (huc_fw_size > size - GUC_WOPCM_RESERVED) {
			kprintf("[drm] i915: no room in the GuC's WOPCM share for the HuC\n");
			return -E2BIG;
		}
	}
	g->wopcm_base = base;
	g->wopcm_size = size;
	if (!base || !size)
		return -E2BIG;
	if (locked)
		return 0;
	uint32_t agent = huc_fw_size ? HUC_LOADING_AGENT_GUC : 0;
	guc_write(i915, g, GUC_WOPCM_SIZE, size);
	guc_write(i915, g, DMA_GUC_WOPCM_OFFSET, base | agent);
	size_reg = guc_read(i915, g, GUC_WOPCM_SIZE);
	off_reg = guc_read(i915, g, DMA_GUC_WOPCM_OFFSET);
	if ((size_reg & (GUC_WOPCM_SIZE_MASK | GUC_WOPCM_SIZE_LOCKED)) != (size | GUC_WOPCM_SIZE_LOCKED) ||
	    (off_reg & (GUC_WOPCM_OFFSET_MASK | GUC_WOPCM_OFFSET_VALID | agent)) !=
		    (base | agent | GUC_WOPCM_OFFSET_VALID)) {
		kprintf("[drm] i915: WOPCM registers did not take: offset %08x, size %08x\n",
			off_reg, size_reg);
		return -EIO;
	}
	return 0;
}

/* ---- messages over the scratch registers ------------------------------------------------------- */

static uint32_t send_reg(const struct i915_guc *g, uint32_t i)
{
	return g->send_base + 4 * i;
}

/* The doorbell: on Gen11 on the value written goes to the firmware as a
 * payload, which treats every value as "messages waiting". */
static void guc_notify(struct i915_device *i915, struct i915_guc *g)
{
	i915_write32(i915, g->notify_reg, GUC_SEND_TRIGGER);
}

/* A request over the message registers (the transport's own set-up, and
 * anything before the transport is open): `len' dwords out, request[0]
 * the HXG header.  The answer is in the same registers: busy first, or
 * asked to retry, then success or failure.  Returns the response's data
 * (or, with `resp', the number of dwords copied there), or an error. */
static int guc_send_mmio(struct i915_device *i915, struct i915_guc *g, const uint32_t *request,
			 uint32_t len, uint32_t *resp, uint32_t nresp)
{
	uint32_t hdr = 0;
	int retries = 0;

	if (!len || len > g->send_count)
		return -EINVAL;
retry:
	for (uint32_t i = 0; i < len; i++)
		i915_write32(i915, send_reg(g, i), request[i]);
	(void)i915_read32(i915, send_reg(g, len - 1));
	guc_notify(i915, g);

	/* No command should take more than 10 ms; a fast one 10 us. */
	int t;
	for (t = 0; t < 1000; t++) {
		hdr = i915_read32(i915, send_reg(g, 0));
		if (hdr & HXG_ORIGIN_GUC)
			break;
		lapic_delay_us(10);
	}
	if (!(hdr & HXG_ORIGIN_GUC))
		goto timeout;
	if (HXG_TYPE(hdr) == HXG_TYPE_NO_RESPONSE_BUSY) {
		for (t = 0; t < 100000; t++) { /* up to a second */
			hdr = i915_read32(i915, send_reg(g, 0));
			if (!(hdr & HXG_ORIGIN_GUC) || HXG_TYPE(hdr) != HXG_TYPE_NO_RESPONSE_BUSY)
				break;
			lapic_delay_us(10);
		}
		if ((hdr & HXG_ORIGIN_GUC) && HXG_TYPE(hdr) == HXG_TYPE_NO_RESPONSE_BUSY)
			goto timeout;
		if (!(hdr & HXG_ORIGIN_GUC))
			goto proto;
	}
	if (HXG_TYPE(hdr) == HXG_TYPE_NO_RESPONSE_RETRY) {
		i915_dbg("[drm] i915: %s: request %08x: retry, reason %u\n", guc_name(g), request[0],
			 HXG_RETRY_REASON(hdr));
		if (++retries < 100)
			goto retry;
		goto timeout;
	}
	if (HXG_TYPE(hdr) == HXG_TYPE_RESPONSE_FAILURE) {
		kprintf("[drm] i915: %s refused request %04x: error %x, hint %u\n", guc_name(g),
			request[0] & 0xffff, HXG_FAILURE_ERROR(hdr), HXG_FAILURE_HINT(hdr));
		return -ENXIO;
	}
	if (HXG_TYPE(hdr) != HXG_TYPE_RESPONSE_SUCCESS)
		goto proto;
	if (resp) {
		uint32_t n = nresp < g->send_count ? nresp : g->send_count;
		if (!n)
			return 0;
		resp[0] = hdr;
		for (uint32_t i = 1; i < n; i++)
			resp[i] = i915_read32(i915, send_reg(g, i));
		return (int)n;
	}
	return (int)HXG_RESPONSE_DATA0(hdr);
timeout:
	kprintf("[drm] i915: %s did not answer request %04x (%08x)\n", guc_name(g),
		request[0] & 0xffff, hdr);
	return -ETIMEDOUT;
proto:
	kprintf("[drm] i915: %s answered request %04x unexpectedly (%08x)\n", guc_name(g),
		request[0] & 0xffff, hdr);
	return -EPROTO;
}

static uint32_t hxg_request(uint32_t action)
{
	return (HXG_TYPE_REQUEST << HXG_TYPE_SHIFT) | (action & HXG_REQUEST_ACTION_MASK);
}

/* One configuration key over the message registers; the answer's data
 * is the number of keys taken. */
static int guc_self_cfg(struct i915_device *i915, struct i915_guc *g, uint16_t key, uint16_t len,
			uint64_t value)
{
	uint32_t req[4] = {
		hxg_request(GUC_ACTION_HOST2GUC_SELF_CFG),
		KLV(key, len),
		(uint32_t)value,
		(uint32_t)(value >> 32),
	};
	int ret = guc_send_mmio(i915, g, req, 4, NULL, 0);
	if (ret == 1)
		return 0;
	if (ret >= 0)
		ret = ret ? -EPROTO : -EINVAL;
	kprintf("[drm] i915: %s did not take configuration key %04x (%d)\n", guc_name(g), key, ret);
	return ret;
}

/* ---- the additional data structures ------------------------------------------------------------- */

#define REGSET_MAX 4096 /* entries for all engines of one GT */

/* Register steering for the save/restore list: the GuC has no default
 * of its own, so every register given the multicast form names a group
 * and instance that exist.  The ranges whose units are counted
 * differently are steered at the first unit of their kind; the rest at
 * the GT's default. */
struct steer_range {
	uint32_t start, end;
};

static const struct steer_range icl_l3bank_ranges[] = {
	{ 0x00b100, 0x00b3ff },
	{ 0, 0 },
};
static const struct steer_range dg2_mslice_ranges[] = {
	{ 0x00dd00, 0x00ddff },
	{ 0x00e900, 0x00ffff },
	{ 0, 0 },
};
static const struct steer_range dg2_lncf_ranges[] = {
	{ 0x00b000, 0x00b0ff },
	{ 0x00d880, 0x00d8ff },
	{ 0, 0 },
};
static const struct steer_range xelpg_instance0_ranges[] = {
	{ 0x000b00, 0x000bff },
	{ 0x001000, 0x001fff },
	{ 0x004000, 0x0048ff },
	{ 0x008700, 0x0087ff },
	{ 0x00b000, 0x00b0ff },
	{ 0x00c800, 0x00cfff },
	{ 0x00d880, 0x00d8ff },
	{ 0x00dd00, 0x00ddff },
	{ 0, 0 },
};
static const struct steer_range xelpg_l3bank_ranges[] = {
	{ 0x00b100, 0x00b3ff },
	{ 0, 0 },
};
static const struct steer_range xelpg_dss_ranges[] = {
	{ 0x005200, 0x0052ff },
	{ 0x005500, 0x007fff },
	{ 0x008140, 0x00815f },
	{ 0x0094d0, 0x00955f },
	{ 0x009680, 0x0096ff },
	{ 0x00d800, 0x00d87f },
	{ 0x00dc00, 0x00dcff },
	{ 0x00de80, 0x00e8ff },
	{ 0, 0 },
};
/* the media GT's, as absolute offsets */
static const struct steer_range xelpmp_oaddrm_ranges[] = {
	{ 0x393200, 0x39323f },
	{ 0x393400, 0x3934ff },
	{ 0, 0 },
};

static int in_ranges(const struct steer_range *r, uint32_t reg)
{
	for (; r->end; r++)
		if (reg >= r->start && reg <= r->end)
			return 1;
	return 0;
}

static uint32_t guc_steering(struct i915_device *i915, const struct i915_guc *g, uint32_t reg)
{
	uint8_t group = 0, inst = 0;

	/* Xe2 on: the steering of the register access code itself; a
	 * register in none of the GT's steering ranges is not steered */
	if (i915->gt_ip >= I915_IP(20, 0)) {
		if (!i915_mcr_steering(i915, i915_gt_reg(g->gsi, reg), &group, &inst))
			return 0;
		return GUC_REGSET_STEERING(group, inst);
	}

	if (g->media) {
		/* the media slice 0 unless all of it is fused off */
		if (in_ranges(xelpmp_oaddrm_ranges, i915_gt_reg(g->gsi, reg)))
			group = ((g->engine_mask & (I915_ENGINE_VCS0 | I915_ENGINE_VECS0)) ||
				 (g->vdbox_sfc_access & 1)) ? 0 : 1;
		return GUC_REGSET_STEERING(group, inst);
	}
	group = i915->mcr_group;
	inst = i915->mcr_instance;
	if (i915->gt_ip >= I915_IP(12, 70)) {
		if (in_ranges(xelpg_l3bank_ranges, reg)) {
			group = 0;
			inst = (uint8_t)first_bit(i915->l3bank_mask);
		} else if (in_ranges(xelpg_dss_ranges, reg)) {
			unsigned dss = first_bit(i915->dss_geometry | i915->dss_compute);
			group = (uint8_t)(dss / 4);
			inst = (uint8_t)(dss % 4);
		} else if (in_ranges(xelpg_instance0_ranges, reg)) {
			group = 0;
			inst = 0;
		}
	} else if (i915->info->platform == I915_PLATFORM_DG2) {
		if (in_ranges(dg2_mslice_ranges, reg)) {
			group = (uint8_t)first_bit(i915->mslice_mask);
			inst = 0;
		} else if (in_ranges(dg2_lncf_ranges, reg)) {
			group = (uint8_t)(first_bit(i915->mslice_mask) << 1);
			inst = 0;
		}
	} else if (i915->info->gen >= 11 && i915->gt_ip < I915_IP(12, 55)) {
		if (in_ranges(icl_l3bank_ranges, reg)) {
			group = 0;
			inst = (uint8_t)first_bit(i915->l3bank_mask);
		}
	}
	return GUC_REGSET_STEERING(group, inst);
}

/* One register into the list of the engine being built, kept sorted; a
 * register already there stays as it is. */
static int regset_add(struct guc_mmio_reg *list, uint32_t *count, uint32_t *total, uint32_t offset,
		      uint32_t flags)
{
	uint32_t n = *count, i;

	for (i = 0; i < n; i++) {
		if (list[i].offset == offset)
			return 0;
		if (list[i].offset > offset)
			break;
	}
	if (*total >= REGSET_MAX)
		return -ENOSPC;
	for (uint32_t k = n; k > i; k--)
		list[k] = list[k - 1];
	list[i].offset = offset;
	list[i].value = 0;
	list[i].flags = flags;
	list[i].mask = 0;
	(*count)++;
	(*total)++;
	return 0;
}

/* What the GuC restores of an engine after it resets it: the engine's
 * mode, status page and interrupt mask, the render unit's mode on the
 * first render or compute engine, every register the engine's
 * workaround list programs (steered, and masked where the register
 * is), every slot of the client whitelist, the L3 cache-control table
 * and the EU performance counters' controls. */
static int regset_build(struct i915_device *i915, struct i915_guc *g)
{
	struct guc_mmio_reg *store = kcalloc(REGSET_MAX, sizeof(*store));
	struct i915_wa *wa_store = kcalloc(256, sizeof(*wa_store));
	uint32_t total = 0;
	int rc = 0;

	if (!store || !wa_store) {
		kfree(store);
		kfree(wa_store);
		return -ENOMEM;
	}
	uint32_t rcs = guc_class_mask(g, I915_ENGINE_CLASS_RENDER);
	uint32_t ccs = guc_class_mask(g, I915_ENGINE_CLASS_COMPUTE);
	int xe2 = i915->gt_ip >= I915_IP(20, 0);
	for (unsigned d = 0; d < GUC_NUM_ENGINE_DESCS && !rc; d++) {
		const struct guc_engine_desc *ed = &guc_engines[d];
		struct guc_mmio_reg *list = store + total;
		uint32_t count = 0;
		uint32_t base = ed->base;

		g->regset_count[ed->id] = 0;
		if (!(g->engine_mask & ed->bit))
			continue;
		rc |= regset_add(list, &count, &total, RING_MODE_GEN7(base), GUC_REGSET_MASKED);
		rc |= regset_add(list, &count, &total, RING_HWS_PGA(base), 0);
		rc |= regset_add(list, &count, &total, RING_IMR(base), 0);
		/* the first render or compute engine: the render domain's
		 * unit mode; on Xe2 also the CCS mode and the status page
		 * mask the engine set-up writes */
		int first_rc = (ed->class == I915_ENGINE_CLASS_RENDER ||
				ed->class == I915_ENGINE_CLASS_COMPUTE) &&
			       (rcs ? ed->class == I915_ENGINE_CLASS_RENDER :
				      first_bit(ccs) == ed->instance);
		if (xe2) {
			rc |= regset_add(list, &count, &total, RING_HWSTAM(base), 0);
			if (first_rc)
				rc |= regset_add(list, &count, &total, GEN12_RCU_MODE,
						 GUC_REGSET_MASKED);
			if (first_rc && i915->ccs_mode)
				rc |= regset_add(list, &count, &total, XEHP_CCS_MODE,
						 GUC_REGSET_MASKED);
		} else if ((ed->class == I915_ENGINE_CLASS_RENDER ||
			    ed->class == I915_ENGINE_CLASS_COMPUTE) &&
			   first_bit(rcs | ccs) == ed->instance && ccs) {
			rc |= regset_add(list, &count, &total, GEN12_RCU_MODE, GUC_REGSET_MASKED);
		}

		/* the engine's workaround list, through a stand-in for the
		 * engine (the engines are set up after the GuC is loaded) */
		struct i915_engine fe;
		struct i915_wa_list wal;
		mm_memset(&fe, 0, sizeof(fe));
		fe.i915 = i915;
		fe.id = (enum i915_engine_id)ed->id;
		fe.class = ed->class;
		fe.instance = ed->instance;
		fe.present = 1;
		fe.mmio_base = base;
		fe.gsi_offset = g->gsi;
		i915_wa_list_init(&wal, "GuC", wa_store, 256);
		i915_wa_engine_list(i915, &fe, &wal);
		for (unsigned i = 0; i < wal.count; i++) {
			uint32_t reg = wal.list[i].reg;
			uint32_t flags = guc_steering(i915, g, reg);
			if (wal.list[i].flags & I915_WA_MASKED)
				flags |= GUC_REGSET_MASKED;
			rc |= regset_add(list, &count, &total, reg, flags);
		}
		for (unsigned i = 0; i < RING_MAX_NONPRIV_SLOTS; i++)
			rc |= regset_add(list, &count, &total, RING_FORCE_TO_NONPRIV(base, i), 0);
		if (xe2) {
			/* Wa_16023105232, Wa_14025941587 */
			if (i915_engine_wa_idledly(&fe))
				rc |= regset_add(list, &count, &total, XE2_RING_IDLEDLY(base), 0);
			g->regset_count[ed->id] = (uint16_t)count;
			continue;
		}
		for (unsigned i = 0; i < LNCFCMOCS_REG_COUNT; i++) {
			if (i915->gt_ip >= I915_IP(12, 55))
				rc |= regset_add(list, &count, &total, XEHP_LNCFCMOCS(i),
						 guc_steering(i915, g, XEHP_LNCFCMOCS(i)));
			else
				rc |= regset_add(list, &count, &total, GEN9_LNCFCMOCS(i), 0);
		}
		if (i915->info->gen >= 12) {
			static const uint32_t perf[] = { EU_PERF_CNTL0, EU_PERF_CNTL1, EU_PERF_CNTL2,
							 EU_PERF_CNTL3, EU_PERF_CNTL4, EU_PERF_CNTL5,
							 EU_PERF_CNTL6 };
			for (unsigned i = 0; i < sizeof(perf) / sizeof(perf[0]); i++)
				rc |= regset_add(list, &count, &total, perf[i],
						 guc_steering(i915, g, perf[i]));
		}
		g->regset_count[ed->id] = (uint16_t)count;
	}
	kfree(wa_store);
	if (rc) {
		kfree(store);
		kprintf("[drm] i915: %s register save/restore list too long\n", guc_name(g));
		return -ENOSPC;
	}
	g->regset = store;
	g->ads_regset_size = total * (uint32_t)sizeof(struct guc_mmio_reg);
	return 0;
}

/* The golden contexts, in the order the GuC is given them: render,
 * video, video enhancement, copy, compute. */
static const uint8_t golden_order[5] = {
	I915_ENGINE_CLASS_RENDER, I915_ENGINE_CLASS_VIDEO, I915_ENGINE_CLASS_VIDEO_ENHANCE,
	I915_ENGINE_CLASS_COPY, I915_ENGINE_CLASS_COMPUTE,
};

/* The size of a class's context image as the hardware uses it: the
 * per-process status page and the register state, not the context's
 * own restore batch pages. */
static uint32_t golden_real_size(struct i915_device *i915, int uapi_class)
{
	struct i915_lrc_layout l;
	if (i915_lrc_layout(i915->info->gen_x10, uapi_class, &l) != 0)
		return 0;
	return (uint32_t)l.hw_pages * 4096;
}

static uint32_t golden_skip(struct i915_device *i915)
{
	return 4096 + (i915->gt_ip >= I915_IP(12, 55) ? XEHP_LR_HW_CONTEXT_BYTES : LR_HW_CONTEXT_BYTES);
}

/* Lay the object out and allocate it. */
static int ads_create(struct i915_device *i915, struct i915_guc *g)
{
	int sub = guc_wants_submission(i915);
	uint32_t off;

	if (sub) {
		int rc = regset_build(i915, g);
		if (rc)
			return rc;
	} else {
		g->ads_regset_size = 0;
	}
	g->ads_regset_off = (uint32_t)sizeof(struct guc_ads_blob);
	off = (g->ads_regset_off + g->ads_regset_size + 4095) & ~4095u;
	/* the golden contexts: room for each class present */
	g->ads_golden_off = off;
	for (int c = 0; c < 5; c++)
		g->golden_off[c] = g->golden_bytes[c] = 0;
	if (sub) {
		for (int k = 0; k < 5; k++) {
			int c = golden_order[k];
			if (!guc_class_mask(g, c))
				continue;
			uint32_t real = golden_real_size(i915, c);
			if (!real)
				continue;
			g->golden_off[c] = off;
			g->golden_bytes[c] = real;
			off += (real + 4095) & ~4095u;
		}
	}
	g->ads_golden_size = off - g->ads_golden_off;
	/* a page for the workaround KLVs, a page for the empty capture list */
	g->ads_waklv_off = off;
	off += 4096;
	g->ads_capture_off = off;
	off += 4096;
	g->ads_private_off = off;
	off += (g->private_data_size + 4095) & ~4095u;
	g->ads_size = off;
	g->ads_obj = guc_alloc(i915, g, off, &g->ads_ggtt, &g->ads_vaddr);
	if (!g->ads_obj)
		return -ENOMEM;
	return 0;
}

static void waklv_add_data(uint8_t *at, uint32_t *used, uint32_t key, const uint32_t *data,
			   uint32_t n)
{
	if (*used + 4 + n * 4 > 4096)
		return;
	uint32_t klv = KLV(key, n);
	mm_memcpy(at + *used, &klv, 4);
	if (n)
		mm_memcpy(at + *used + 4, data, n * 4);
	*used += 4 + n * 4;
}

static void waklv_add(uint8_t *at, uint32_t *used, uint32_t key)
{
	waklv_add_data(at, used, key, NULL, 0);
}

/* Xe2 on: the workarounds the GuC takes as KLVs, by the IP of the GuC's
 * GT (graphics rules for the primary GuC, media rules for the media
 * GuC) and the firmware's version. */
static void waklv_xe2(struct i915_device *i915, const struct i915_guc *g, uint8_t *at,
		      uint32_t *used)
{
	unsigned gip = g->media ? 0 : i915->gt_ip;
	unsigned mip = g->media ? i915->media_ip : 0;
	uint32_t fw = g->fw_version;
	int gfx_a = !g->media && i915->gt_step < I915_STEP_B0;
	int media_a = g->media && i915->media_step < I915_STEP_B0;

	/* Wa_18024947630 */
	if (gip == I915_IP(20, 1) || gip == I915_IP(20, 4) || mip == I915_IP(20, 0))
		waklv_add(at, used, KLV_WA_GAM_PFQ_SHADOW_TAIL_POLLING);
	/* Wa_16022287689 */
	if (gip == I915_IP(20, 1) || gip == I915_IP(20, 4))
		waklv_add(at, used, KLV_WA_DISABLE_MTP_DURING_ASYNC_COMPUTE);
	/* Wa_14022866841 */
	if ((gip == I915_IP(30, 0) && gfx_a) || (mip == I915_IP(30, 0) && media_a))
		waklv_add(at, used, KLV_WA_WAKE_POWER_DOMAINS_FOR_OUTBOUND_MMIO);
	/* Wa_13011645652: the value the GuC writes back at the late
	 * restore of render C-state exit */
	if (gip >= I915_IP(20, 4) && gip <= I915_IP(30, 5)) {
		uint32_t v = 0xc40;
		waklv_add_data(at, used, KLV_WA_NP_RD_WRITE_TO_CLEAR_RCSM_AT_CGP_LATE_RESTORE, &v, 1);
	}
	/* Wa_14022293748, Wa_22019794406 */
	if (gip >= I915_IP(20, 1) && gip <= I915_IP(30, 5))
		waklv_add(at, used, KLV_WA_BACK_TO_BACK_RCS_ENGINE_RESET);
	/* Wa_16026508708 */
	if (fw >= GUC_VER(70, 44, 0) &&
	    ((gip >= I915_IP(12, 0) && gip <= I915_IP(30, 5)) ||
	     (mip >= I915_IP(13, 0) && mip <= I915_IP(30, 2))))
		waklv_add(at, used, KLV_WA_RESET_BB_STACK_PTR_ON_VF_SWITCH);
	/* Wa_16026007364 */
	if (fw >= GUC_VER(70, 47, 0) && mip == I915_IP(30, 0)) {
		uint32_t v[2] = { 0x0, 0xf };
		waklv_add_data(at, used, KLV_WA_RESTORE_UNSAVED_MEDIA_CONTROL_REG, v, 2);
	}
	/* Wa_14025515070 */
	if (fw >= GUC_VER(70, 53, 0) &&
	    (gip == I915_IP(20, 4) || (mip >= I915_IP(13, 1) && mip <= I915_IP(30, 0)) ||
	     mip == I915_IP(30, 2) || (gip >= I915_IP(30, 0) && gip <= I915_IP(30, 1)) ||
	     (gip >= I915_IP(30, 3) && gip <= I915_IP(30, 5)) || mip == I915_IP(35, 0) ||
	     (gip == I915_IP(35, 10) && gfx_a)))
		waklv_add(at, used, KLV_WA_CLR_CS_INDIRECT_RING_STATE_IF_IDLE_AT_CTX_REG);
	/* Wa_22022079272 */
	if (fw >= GUC_VER(70, 62, 0) &&
	    (mip == I915_IP(35, 3) || gip == I915_IP(35, 10) || gip == I915_IP(35, 11)))
		waklv_add(at, used, KLV_WA_REMAP_RANGED_TLB_INV);
	/* Wa_16029897822: not on Nova Lake-S, whose GuC does not enable
	 * the 64-bit semaphore tokens */
	if (fw >= GUC_VER(70, 69, 0) && i915->info->platform != I915_PLATFORM_NOVALAKE_S &&
	    (mip == I915_IP(35, 0) || gip == I915_IP(35, 10)))
		waklv_add(at, used, KLV_WA_IGNORE_MMIO_READ_SEM_TOKEN_64);
}

/* Fill in everything but the golden contexts' contents (which survive a
 * reload); the firmware's private data starts out empty every load. */
static void ads_populate(struct i915_device *i915, struct i915_guc *g)
{
	int sub = guc_wants_submission(i915);
	struct guc_ads_blob *b = (struct guc_ads_blob *)g->ads_vaddr;
	uint32_t base = g->ads_ggtt;

	mm_memset(g->ads_vaddr, 0, g->ads_golden_off);
	mm_memset(g->ads_vaddr + g->ads_waklv_off, 0, g->ads_private_off - g->ads_waklv_off);
	mm_memset(g->ads_vaddr + g->ads_private_off, 0, g->ads_size - g->ads_private_off);

	/* the scheduling policies: the firmware's defaults, engine resets
	 * allowed */
	b->policies.dpc_promote_time = GLOBAL_POLICY_DEFAULT_DPC_PROMOTE_TIME_US;
	b->policies.max_num_work_items = GLOBAL_POLICY_MAX_NUM_WI;
	b->policies.global_flags = 0;
	b->policies.is_valid = 1;

	/* the GT: which engines exist, and the physical instance behind
	 * each logical one */
	for (int c = 0; c < GUC_MAX_ENGINE_CLASSES; c++)
		for (int n = 0; n < GUC_MAX_INSTANCES_PER_CLASS; n++)
			b->system_info.mapping_table[c][n] = GUC_MAX_INSTANCES_PER_CLASS;
	for (int c = 0; c < 5; c++) {
		uint32_t m = guc_class_mask(g, c);
		int gc = guc_class_of(c);
		b->system_info.engine_enabled_masks[gc] = m;
		for (unsigned i = 0; i < 8; i++)
			if (m & (1u << i))
				b->system_info.mapping_table[gc][guc_logical_instance(i915, g, c, i)] =
					(uint8_t)i;
	}
	b->system_info.generic_gt_sysinfo[GUC_GENERIC_GT_SYSINFO_SLICE_ENABLED] =
		i915->gt_ip >= I915_IP(12, 55) ? 1 : popcount32(i915->slice_mask & 0xff);
	b->system_info.generic_gt_sysinfo[GUC_GENERIC_GT_SYSINFO_VDBOX_SFC_SUPPORT_MASK] =
		g->vdbox_sfc_access;
	if (i915->info->gen >= 12 && !(i915->info->flags & I915_INFO_IS_DGFX)) {
		uint32_t dbs = guc_read(i915, g, GEN12_DIST_DBS_POPULATED);
		b->system_info.generic_gt_sysinfo[GUC_GENERIC_GT_SYSINFO_DOORBELL_COUNT_PER_SQIDI] =
			((dbs >> GEN12_DOORBELLS_PER_SQIDI_SHIFT) & GEN12_DOORBELLS_PER_SQIDI) + 1;
	}

	/* the golden contexts: the whole image's address, the size of the
	 * engine state after what every image starts with */
	for (int c = 0; c < 5; c++) {
		if (!g->golden_bytes[c])
			continue;
		int gc = guc_class_of(c);
		b->ads.golden_context_lrca[gc] = base + g->golden_off[c];
		b->ads.eng_state_size[gc] = g->golden_bytes[c] - golden_skip(i915);
	}

	/* error capture: no register lists, every one pointing at the same
	 * empty list (a zero header) */
	uint32_t null_list = base + g->ads_capture_off;
	for (int i = 0; i < GUC_CAPTURE_LIST_INDEX_MAX; i++) {
		for (int j = 0; j < GUC_MAX_ENGINE_CLASSES; j++) {
			b->ads.capture_class[i][j] = null_list;
			b->ads.capture_instance[i][j] = null_list;
		}
		b->ads.capture_global[i] = null_list;
	}

	b->ads.scheduler_policies = base + (uint32_t)__builtin_offsetof(struct guc_ads_blob, policies);
	b->ads.gt_system_info = base + (uint32_t)__builtin_offsetof(struct guc_ads_blob, system_info);

	/* the register save/restore lists, one after another */
	if (g->regset && g->ads_regset_size) {
		uint32_t addr = base + g->ads_regset_off;
		mm_memcpy(g->ads_vaddr + g->ads_regset_off, g->regset, g->ads_regset_size);
		for (unsigned d = 0; d < GUC_NUM_ENGINE_DESCS; d++) {
			const struct guc_engine_desc *ed = &guc_engines[d];
			uint32_t count = g->regset_count[ed->id];
			if (!(g->engine_mask & ed->bit) || !count)
				continue;
			struct guc_mmio_reg_set *set =
				&b->ads.reg_state_list[guc_class_of(ed->class)][ed->instance];
			set->address = addr;
			set->count = (uint16_t)count;
			addr += count * (uint32_t)sizeof(struct guc_mmio_reg);
		}
	}

	/* the workaround KLVs the firmware takes from 70.10 on */
	if (sub && g->fw_version >= GUC_VER(70, 10, 0) && i915->gt_ip >= I915_IP(20, 0)) {
		uint8_t *at = g->ads_vaddr + g->ads_waklv_off;
		uint32_t used = 0;
		waklv_xe2(i915, g, at, &used);
		if (used) {
			b->ads.wa_klv_addr_lo = base + g->ads_waklv_off;
			b->ads.wa_klv_addr_hi = 0;
			b->ads.wa_klv_size = used;
		}
	} else if (sub && g->fw_version >= GUC_VER(70, 10, 0)) {
		uint8_t *at = g->ads_vaddr + g->ads_waklv_off;
		uint32_t used = 0;
		int gfx_127x = !g->media && i915->gt_ip >= I915_IP(12, 70) &&
			       i915->gt_ip <= I915_IP(12, 74);
		/* Wa_14019159160 */
		if (gfx_127x) {
			waklv_add(at, &used, KLV_WA_SERIALIZED_RA_MODE);
			waklv_add(at, &used, KLV_WA_AVOID_GFX_CLEAR_WHILE_ACTIVE);
		}
		/* Wa_16021333562 */
		if (g->fw_version >= GUC_VER(70, 21, 1) &&
		    (gfx_127x || (g->media && i915->media_ip == I915_IP(13, 0)) ||
		     i915->info->platform == I915_PLATFORM_DG2))
			waklv_add(at, &used, KLV_WA_BLOCK_INTERRUPTS_WHEN_MGSR_BLOCKED);
		if (used) {
			b->ads.wa_klv_addr_lo = base + g->ads_waklv_off;
			b->ads.wa_klv_addr_hi = 0;
			b->ads.wa_klv_size = used;
		}
	}

	b->ads.private_data = base + g->ads_private_off;
	clflush_range(g->ads_vaddr, g->ads_size);
}

void i915_guc_ads_update_golden(struct i915_device *i915)
{
	struct i915_guc *gucs[2] = { &i915->guc, &i915->media_guc };

	if (!guc_wants_submission(i915))
		return;
	for (int k = 0; k < 2; k++) {
		struct i915_guc *g = gucs[k];
		if (!g->loaded || !g->ads_obj)
			continue;
		struct guc_ads_blob *b = (struct guc_ads_blob *)g->ads_vaddr;
		int copied = 0;
		for (int c = 0; c < 5; c++) {
			if (!g->golden_bytes[c])
				continue;
			struct drm_gem_object *o = NULL;
			for (int i = 0; i < I915_NUM_ENGINES && !o; i++) {
				struct i915_engine *e = &i915->engines[i];
				if (!e->present || e->class != c || e->gsi_offset != g->gsi || !e->kctx ||
				    !e->golden_ready)
					continue;
				o = e->kctx->lrc[e->id].obj;
			}
			int gc = guc_class_of(c);
			if (!o) {
				kprintf("[drm] i915: %s: no default state recorded for engine class %d\n",
					guc_name(g), c);
				b->ads.eng_state_size[gc] = 0;
				b->ads.golden_context_lrca[gc] = 0;
				continue;
			}
			uint32_t pages = g->golden_bytes[c] / 4096;
			uint8_t *dst = g->ads_vaddr + g->golden_off[c];
			for (uint32_t p = 0; p < pages; p++) {
				if (p < o->npages) {
					uint8_t *src = phys_to_virt(o->pages[p]);
					if (!(i915->info->flags & I915_INFO_HAS_LLC))
						clflush_range(src, 4096);
					mm_memcpy(dst + p * 4096, src, 4096);
				} else {
					mm_memset(dst + p * 4096, 0, 4096);
				}
			}
			b->ads.golden_context_lrca[gc] = g->ads_ggtt + g->golden_off[c];
			b->ads.eng_state_size[gc] = g->golden_bytes[c] - golden_skip(i915);
			copied++;
		}
		clflush_range(g->ads_vaddr, g->ads_size);
		g->golden_filled = 1;
		i915_dbg("[drm] i915: %s: %d golden contexts recorded\n", guc_name(g), copied);
	}
}

/* ---- the firmware's log ------------------------------------------------------------------------- */

static int log_create(struct i915_device *i915, struct i915_guc *g)
{
	int uc = g->media && i915->media_ip == I915_IP(13, 0);
	g->log_obj = i915_gem_alloc_bound(i915, GUC_LOG_BYTES, uc, 0, &g->log_ggtt);
	if (!g->log_obj)
		return -ENOMEM;
	for (uint32_t p = 0; p < g->log_obj->npages; p++) {
		uint8_t *va = phys_to_virt(g->log_obj->pages[p]);
		mm_memset(va, 0, 4096);
		clflush_range(va, 4096);
	}
	return 0;
}

/* Valid, the crash and debug sections in 4 KB units (count minus one),
 * the capture section in 1 MB units, and the buffer's page. */
static uint32_t guc_ctl_log_params(const struct i915_guc *g)
{
	uint32_t crash = GUC_LOG_CRASH_BYTES / 4096 - 1;
	uint32_t debug = GUC_LOG_DEBUG_BYTES / 4096 - 1;
	uint32_t capture = GUC_LOG_CAPTURE_BYTES / (1024 * 1024) - 1;
	return GUC_LOG_VALID | GUC_LOG_NOTIFY_ON_HALF_FULL | GUC_LOG_CAPTURE_ALLOC_UNITS |
	       (crash << GUC_LOG_CRASH_SHIFT) | (debug << GUC_LOG_DEBUG_SHIFT) |
	       (capture << GUC_LOG_CAPTURE_SHIFT) |
	       ((g->log_ggtt >> 12) << GUC_LOG_BUF_ADDR_SHIFT);
}

/* ---- the upload ----------------------------------------------------------------------------------- */

/* Wa_14025883347: a GuC that runs is reset only once its SRAM has been
 * saved and restored, or its firmware's next DMA fails: the idle flow
 * off (the reset turns it on again), the GuC ready, the SRAM idle. */
#define GUC_BOOT_HASH_CHK 0xc010
#define GUC_BOOT_UKERNEL_VALID (1u << 31)
#define GUC_SRAM_STATUS 0xc398
#define GUC_SRAM_HANDLING_MASK (3u << 7)
#define GUC_MAX_IDLE_COUNT 0xc3e4
#define GUC_IDLE_FLOW_DISABLE (1u << 31)

static int wa_14025883347(const struct i915_device *i915, const struct i915_guc *g)
{
	if (g->media)
		return i915->media_ip >= I915_IP(13, 1) && i915->media_ip <= I915_IP(35, 0);
	return i915->gt_ip >= I915_IP(20, 4) && i915->gt_ip <= I915_IP(30, 5);
}

static void guc_prevent_fw_dma_failure_on_reset(struct i915_device *i915, struct i915_guc *g)
{
	uint32_t st = i915_read32_fw(i915, guc_reg(g, GUC_STATUS));

	if (st & GS_MIA_IN_RESET)
		return;
	if (!(i915_read32_fw(i915, guc_reg(g, GUC_BOOT_HASH_CHK)) & GUC_BOOT_UKERNEL_VALID))
		return;
	i915_write32_fw(i915, guc_reg(g, GUC_MAX_IDLE_COUNT),
			i915_read32_fw(i915, guc_reg(g, GUC_MAX_IDLE_COUNT)) | GUC_IDLE_FLOW_DISABLE);
	for (int t = 0; t < 10000; t++) { /* 100 ms */
		st = i915_read32_fw(i915, guc_reg(g, GUC_STATUS));
		if (((st & GS_UKERNEL_MASK) >> GS_UKERNEL_SHIFT) == GUC_LOAD_STATUS_RUNNING)
			break;
		lapic_delay_us(10);
	}
	for (int t = 0; t < 500; t++) { /* 5 ms */
		if (!(i915_read32_fw(i915, guc_reg(g, GUC_SRAM_STATUS)) & GUC_SRAM_HANDLING_MASK))
			break;
		lapic_delay_us(10);
	}
}

static int guc_reset(struct i915_device *i915, struct i915_guc *g)
{
	uint32_t dom = i915->info->gen >= 11 ? GEN11_GRDOM_GUC : GEN9_GRDOM_GUC;
	uint32_t reg = guc_reg(g, GEN6_GDRST);
	int loops = i915->gt_ip < I915_IP(12, 70) ? 2 : 1;
	int rc = 0;

	i915_fw_get(i915, I915_FW_ALL);
	if (wa_14025883347(i915, g))
		guc_prevent_fw_dma_failure_on_reset(i915, g);
	do {
		i915_write32_fw(i915, reg, dom);
		rc = -ETIMEDOUT;
		for (int t = 0; t < 200; t++) { /* 2 ms */
			if (!(i915_read32_fw(i915, reg) & dom)) {
				rc = 0;
				break;
			}
			lapic_delay_us(10);
		}
	} while (rc == 0 && --loops);
	lapic_delay_us(50);
	i915_fw_put(i915, I915_FW_ALL);
	if (rc)
		kprintf("[drm] i915: the %s did not come out of reset\n", guc_name(g));
	return rc;
}

static void guc_prepare_xfer(struct i915_device *i915, struct i915_guc *g)
{
	uint32_t shim = GUC_ENABLE_READ_CACHE_LOGIC | GUC_ENABLE_READ_CACHE_FOR_SRAM_DATA |
			GUC_ENABLE_READ_CACHE_FOR_WOPCM_DATA | GUC_ENABLE_MIA_CLOCK_GATING;

	if (i915->gt_ip < I915_IP(12, 55))
		shim |= GUC_DISABLE_SRAM_INIT_TO_ZEROES | GUC_ENABLE_MIA_CACHING;
	/* Xe2 on: the cache control entry of the GuC's own accesses, the
	 * uncached one */
	if (i915->gt_ip >= I915_IP(20, 0))
		shim |= GUC_MOCS_INDEX(i915_mocs_uc_index(i915));
	/* before the DMA */
	guc_write(i915, g, GUC_SHIM_CONTROL, shim);
	if (i915->info->gen == 9 && (i915->info->flags & I915_INFO_IS_LP))
		guc_write(i915, g, GEN9LP_GT_PM_CONFIG, GT_DOORBELL_ENABLE);
	else
		guc_write(i915, g, GEN9_GT_PM_CONFIG, GT_DOORBELL_ENABLE);
	if (i915->info->gen == 9) {
		/* DOP clock gating for the GuC's clocks; 5 us (in 10 ns
		 * units) before the GT may enter RC6 */
		guc_write(i915, g, GEN7_MISCCPCTL,
			  guc_read(i915, g, GEN7_MISCCPCTL) | GEN8_DOP_CLOCK_GATE_GUC_ENABLE);
		guc_write(i915, g, GUC_ARAT_C6DIS, 0x1ff);
	}
	/* from 12.50 on the GuC's internal state is mirrored into the debug
	 * registers only when asked */
	if (i915->gt_ip >= I915_IP(12, 50))
		guc_write(i915, g, GUC_SHIM_CONTROL2,
			  guc_read(i915, g, GUC_SHIM_CONTROL2) | GUC_ENABLE_DEBUG_REG);
}

/* The DMA of an image's header and code into the WOPCM. */
static int uc_dma(struct i915_device *i915, struct i915_guc *g, uint32_t src_ggtt, uint32_t bytes,
		  uint32_t dst, uint32_t flags)
{
	int rc = -ETIMEDOUT;

	i915_fw_get(i915, I915_FW_ALL);
	i915_write32_fw(i915, guc_reg(g, DMA_ADDR_0_LOW), src_ggtt);
	i915_write32_fw(i915, guc_reg(g, DMA_ADDR_0_HIGH), 0);
	i915_write32_fw(i915, guc_reg(g, DMA_ADDR_1_LOW), dst);
	i915_write32_fw(i915, guc_reg(g, DMA_ADDR_1_HIGH), DMA_ADDRESS_SPACE_WOPCM);
	i915_write32_fw(i915, guc_reg(g, DMA_COPY_SIZE), bytes);
	i915_write32_fw(i915, guc_reg(g, DMA_CTRL), I915_MASKED_ENABLE(flags | START_DMA));
	for (int t = 0; t < 10000; t++) { /* 100 ms */
		if (!(i915_read32_fw(i915, guc_reg(g, DMA_CTRL)) & START_DMA)) {
			rc = 0;
			break;
		}
		lapic_delay_us(10);
	}
	if (rc)
		kprintf("[drm] i915: %s firmware DMA did not finish (DMA_CTRL %08x)\n",
			flags & HUC_UKERNEL ? "HuC" : guc_name(g),
			i915_read32_fw(i915, guc_reg(g, DMA_CTRL)));
	i915_write32_fw(i915, guc_reg(g, DMA_CTRL), I915_MASKED_DISABLE(flags));
	i915_fw_put(i915, I915_FW_ALL);
	return rc;
}

static uint32_t guc_ctl_wa_flags(struct i915_device *i915, const struct i915_guc *g)
{
	unsigned ip = i915->gt_ip;
	int gfx = !g->media;
	int dg2 = i915->info->platform == I915_PLATFORM_DG2;
	int a_step_1270 = gfx && ip == I915_IP(12, 70) && i915->gt_step != I915_STEP_NONE &&
			  i915->gt_step < I915_STEP_B0;
	uint32_t flags = 0;

	/* Xe2 on: of the workarounds the GuC takes as flags only the dual
	 * queue applies -- on the GT with compute engines, which the
	 * hardware would otherwise stall a context switch for while another
	 * address space runs on one of them */
	if (ip >= I915_IP(20, 0)) {
		if (gfx && guc_class_mask(g, I915_ENGINE_CLASS_COMPUTE))
			flags |= GUC_WA_DUAL_QUEUE;
		return flags;
	}

	/* Wa_22012773006: Gen11 and Gen12 before Xe_HP */
	if (i915->info->gen >= 11 && ip < I915_IP(12, 55))
		flags |= GUC_WA_POLLCS;
	/* Wa_14014475959 */
	if (a_step_1270 || dg2)
		flags |= GUC_WA_HOLD_CCS_SWITCHOUT;
	/* Wa_16019325821, Wa_14019159160 */
	if (gfx && ip >= I915_IP(12, 70) && ip <= I915_IP(12, 74))
		flags |= GUC_WA_RCS_CCS_SWITCHOUT;
	/* Wa_14012197797, Wa_22011391025: the GuC schedules so as to avoid
	 * the stalls later parts prevent in hardware */
	if (dg2 || (guc_class_mask(g, I915_ENGINE_CLASS_COMPUTE) && ip >= I915_IP(12, 70)))
		flags |= GUC_WA_DUAL_QUEUE;
	/* Wa_22011802037: Gen11 and Gen12 up to the first stepping of 12.70 */
	if (i915->info->gen >= 11 && (a_step_1270 || ip < I915_IP(12, 70)))
		flags |= GUC_WA_PRE_PARSER;
	/* Wa_22012727170, Wa_22012727685 */
	if (dg2 && i915->subplatform == I915_SUBPLATFORM_DG2_G11)
		flags |= GUC_WA_CONTEXT_ISOLATION;
	/* Wa_14018913170 */
	if (g->fw_version >= GUC_VER(70, 7, 0))
		flags |= GUC_WA_ENABLE_TSC_CHECK_ON_RC6;
	return flags;
}

/* Xe3P on: the GT's page fault reports go to the queues of the main GAM
 * controller -- not on a media GT whose engines and GuC are still Xe3's
 * (its GMD_ID names a sub-IP). */
static int guc_main_gamctrl_queues(const struct i915_device *i915, const struct i915_guc *g)
{
	if (g->media)
		return i915->media_ip >= I915_IP(35, 0) && i915->media_ip < I915_IP(36, 0) ?
			       !i915->media_subip : i915->media_ip >= I915_IP(36, 0);
	return i915->gt_ip >= I915_IP(35, 0);
}

static uint32_t guc_ctl_feature_flags(struct i915_device *i915, const struct i915_guc *g)
{
	uint32_t flags = (guc_wants_submission(i915) ? 0 : GUC_CTL_DISABLE_SCHEDULER) |
			 (guc_wants_slpc(i915) ? GUC_CTL_ENABLE_SLPC : 0);

	if (i915->gt_ip < I915_IP(20, 0))
		return flags;
	flags |= GUC_CTL_ENABLE_LITE_RESTORE;
	if (guc_main_gamctrl_queues(i915, g))
		flags |= GUC_CTL_MAIN_GAMCTRL_QUEUES;
	/* Xe3P integrated: the media GuC optimises the L2 flush */
	if (g->media && i915->gt_ip >= I915_IP(35, 0) && !(i915->info->flags & I915_INFO_IS_DGFX))
		flags |= GUC_CTL_ENABLE_L2FLUSH_OPT;
	return flags;
}

/* What the firmware reads at start and never again. */
static void guc_write_params(struct i915_device *i915, struct i915_guc *g)
{
	uint32_t p[GUC_CTL_MAX_DWORDS];

	mm_memset(p, 0, sizeof(p));
	p[GUC_CTL_LOG_PARAMS] = guc_ctl_log_params(g);
	p[GUC_CTL_WA] = guc_ctl_wa_flags(i915, g);
	p[GUC_CTL_FEATURE] = guc_ctl_feature_flags(i915, g);
	p[GUC_CTL_DEBUG] = GUC_LOG_DISABLED;
	p[GUC_CTL_ADS] = (g->ads_ggtt >> 12) << GUC_ADS_ADDR_SHIFT;
	p[GUC_CTL_DEVID] = ((uint32_t)i915->devid << 16) | i915->revid;
	for (int i = 0; i < GUC_CTL_MAX_DWORDS; i++)
		i915_dbg("[drm] i915: %s param[%2d] = %08x\n", guc_name(g), i, p[i]);
	guc_write(i915, g, SOFT_SCRATCH(0), 0);
	for (int i = 0; i < GUC_CTL_MAX_DWORDS; i++)
		guc_write(i915, g, SOFT_SCRATCH(1 + i), p[i]);
}

/* Started, or a known failure: 1 with *ok set accordingly; 0 while the
 * load is still in progress. */
static int guc_load_done(uint32_t st, int *ok)
{
	uint32_t uk = (st & GS_UKERNEL_MASK) >> GS_UKERNEL_SHIFT;
	uint32_t br = (st & GS_BOOTROM_MASK) >> GS_BOOTROM_SHIFT;

	switch (uk) {
	case GUC_LOAD_STATUS_RUNNING:
		*ok = 1;
		return 1;
	case GUC_LOAD_STATUS_ERROR_DEVID_BUILD_MISMATCH:
	case GUC_LOAD_STATUS_GUC_PREPROD_BUILD_MISMATCH:
	case GUC_LOAD_STATUS_ERROR_DEVID_INVALID_GUCTYPE:
	case GUC_LOAD_STATUS_HWCONFIG_ERROR:
	case GUC_LOAD_STATUS_DPC_ERROR:
	case GUC_LOAD_STATUS_EXCEPTION:
	case GUC_LOAD_STATUS_INIT_DATA_INVALID:
	case GUC_LOAD_STATUS_MPU_DATA_INVALID:
	case GUC_LOAD_STATUS_INIT_MMIO_SAVE_RESTORE_INVALID:
	case GUC_LOAD_STATUS_KLV_WORKAROUND_INIT_ERROR:
		*ok = 0;
		return 1;
	}
	switch (br) {
	case BOOTROM_STATUS_NO_KEY_FOUND:
	case BOOTROM_STATUS_RSA_FAILED:
	case BOOTROM_STATUS_PAVPC_FAILED:
	case BOOTROM_STATUS_WOPCM_FAILED:
	case BOOTROM_STATUS_LOADLOC_FAILED:
	case BOOTROM_STATUS_JUMP_FAILED:
	case BOOTROM_STATUS_RC6CTXCONFIG_FAILED:
	case BOOTROM_STATUS_MPUMAP_INCORRECT:
	case BOOTROM_STATUS_EXCEPTION:
	case BOOTROM_STATUS_PROD_KEY_CHECK_FAILURE:
		*ok = 0;
		return 1;
	}
	return 0;
}

/* Wait for the firmware to start: some 20 ms at full clock, seconds
 * when the GT runs at its lowest; three seconds at most. */
static int guc_wait_ucode(struct i915_device *i915, struct i915_guc *g)
{
	uint64_t t0 = hrtimer_now_ns();
	uint32_t st = 0;
	int ok = 0, done = 0;

	for (int t = 0; t < 300000 && !done; t++) {
		st = guc_read(i915, g, GUC_STATUS);
		done = guc_load_done(st, &ok);
		if (!done)
			lapic_delay_us(10);
	}
	uint64_t ms = (hrtimer_now_ns() - t0) / 1000000;
	if (done && ok) {
		if (ms > 200)
			kprintf("[drm] i915: %s took %u ms to start\n", guc_name(g), (unsigned)ms);
		else
			i915_dbg("[drm] i915: %s started in %u ms\n", guc_name(g), (unsigned)ms);
		return 0;
	}
	uint32_t uk = (st & GS_UKERNEL_MASK) >> GS_UKERNEL_SHIFT;
	uint32_t br = (st & GS_BOOTROM_MASK) >> GS_BOOTROM_SHIFT;
	int rc = done ? -ENXIO : -ETIMEDOUT;
	const char *why = done ? "failed" : "did not complete";

	switch (br) {
	case BOOTROM_STATUS_NO_KEY_FOUND:
		kprintf("[drm] i915: %s load: invalid key requested (header %08x)\n", guc_name(g),
			guc_read(i915, g, GUC_HEADER_INFO));
		rc = -ENOEXEC;
		break;
	case BOOTROM_STATUS_RSA_FAILED:
		kprintf("[drm] i915: %s load: firmware signature rejected\n", guc_name(g));
		rc = -ENOEXEC;
		break;
	case BOOTROM_STATUS_PROD_KEY_CHECK_FAILURE:
		kprintf("[drm] i915: %s load: production part check failed\n", guc_name(g));
		rc = -ENOEXEC;
		break;
	}
	switch (uk) {
	case GUC_LOAD_STATUS_EXCEPTION:
		kprintf("[drm] i915: %s load: firmware exception at %08x\n", guc_name(g),
			guc_read(i915, g, SOFT_SCRATCH(13)));
		rc = -ENXIO;
		break;
	case GUC_LOAD_STATUS_INIT_MMIO_SAVE_RESTORE_INVALID:
		kprintf("[drm] i915: %s load: illegal register in the save/restore list\n",
			guc_name(g));
		rc = -EPERM;
		break;
	case GUC_LOAD_STATUS_KLV_WORKAROUND_INIT_ERROR:
		kprintf("[drm] i915: %s load: invalid workaround KLV\n", guc_name(g));
		rc = -EINVAL;
		break;
	case GUC_LOAD_STATUS_HWCONFIG_START:
		kprintf("[drm] i915: %s load: still extracting the hardware configuration\n",
			guc_name(g));
		rc = -ETIMEDOUT;
		break;
	}
	kprintf("[drm] i915: %s load %s after %u ms: status %08x (boot ROM %02x, kernel %02x, MIA %x, auth %x)\n",
		guc_name(g), why, (unsigned)ms, st, br, uk, (st & GS_MIA_MASK) >> GS_MIA_SHIFT,
		(st & GS_AUTH_STATUS_MASK) >> GS_AUTH_STATUS_SHIFT);
	return rc;
}

/* The signature: from the 64 scratch registers where the key is 2048
 * bits; from memory (its address in the first register) where larger. */
static void guc_xfer_rsa(struct i915_device *i915, struct i915_guc *g, const struct uc_image *img)
{
	if (g->guc_rsa_ggtt) {
		guc_write(i915, g, UOS_RSA_SCRATCH(0), g->guc_rsa_ggtt);
		return;
	}
	const uint8_t *rsa = (const uint8_t *)img->data + UC_CSS_HEADER_BYTES + img->ucode_size;
	for (int i = 0; i < UOS_RSA_SCRATCH_COUNT; i++)
		guc_write(i915, g, UOS_RSA_SCRATCH(i), rd32(rsa + i * 4));
}

/* ---- the hardware configuration table ------------------------------------------------- */

/* Which parts' GuC has the table: Alder Lake-P (not -N), and every part
 * from Xe_HP on. */
static int guc_has_hwconfig(const struct i915_device *i915)
{
	if (i915->info->platform == I915_PLATFORM_ALDERLAKE_P &&
	    i915->subplatform != I915_SUBPLATFORM_ADL_N)
		return 1;
	return i915->gt_ip >= I915_IP(12, 55);
}

static int guc_get_hwconfig(struct i915_device *i915, struct i915_guc *g, uint32_t ggtt,
			    uint32_t size)
{
	uint32_t req[4] = { hxg_request(GUC_ACTION_GET_HWCONFIG), ggtt, 0, size };
	int rc = guc_send_mmio(i915, g, req, 4, NULL, 0);
	return rc == -ENXIO ? -ENOENT : rc;
}

/* A u32 attribute of the table: key, length in dwords, the value. */
static int hwconfig_lookup_u32(const uint32_t *t, uint32_t dwords, uint32_t key, uint32_t *val)
{
	for (uint32_t i = 0; i + 1 < dwords;) {
		uint32_t len = t[i + 1];
		if (t[i] == key && len >= 1 && i + 2 < dwords) {
			*val = t[i + 2];
			return 0;
		}
		i += 2 + len;
	}
	return -ENOENT;
}

/* The table, read once after the primary GuC started: its size first (a
 * request with no buffer), then into a buffer the GuC reaches, copied for
 * the query of the same name.  Xe2 on, the steering's groups of dual
 * subslices come from it. */
static void guc_hwconfig_fetch(struct i915_device *i915, struct i915_guc *g)
{
	struct drm_gem_object *o;
	uint32_t ggtt;
	uint8_t *va;
	int size, rc;

	if (i915->hwconfig || !guc_has_hwconfig(i915))
		return;
	size = guc_get_hwconfig(i915, g, 0, 0);
	if (size <= 0) {
		i915_dbg("[drm] i915: %s has no hardware configuration table (%d)\n", guc_name(g),
			 size);
		return;
	}
	o = guc_alloc(i915, g, (uint32_t)size, &ggtt, &va);
	if (!o)
		return;
	rc = guc_get_hwconfig(i915, g, ggtt, (uint32_t)size);
	if (rc >= 0) {
		void *copy = kalloc((size_t)size);
		if (copy) {
			clflush_range(va, (uint32_t)size);
			mm_memcpy(copy, va, (size_t)size);
			i915->hwconfig = copy;
			i915->hwconfig_size = (uint32_t)size;
		}
	} else {
		kprintf("[drm] i915: %s did not give its hardware configuration table (%d)\n",
			guc_name(g), rc);
	}
	drm_gem_put(o);
	if (i915->hwconfig && i915->gt_ip >= I915_IP(20, 0)) {
		uint32_t slices = 0, subslices = 0;
		const uint32_t *t = i915->hwconfig;
		if (hwconfig_lookup_u32(t, i915->hwconfig_size / 4, HWCONFIG_ATTR_MAX_SLICES, &slices) ||
		    hwconfig_lookup_u32(t, i915->hwconfig_size / 4, HWCONFIG_ATTR_MAX_SUBSLICES,
					&subslices) ||
		    !slices || !subslices)
			kprintf("[drm] i915: slice and subslice counts missing from the hardware configuration table; steering by groups of four\n");
		else
			i915_mcr_set_dss_per_group(i915, (subslices + slices - 1) / slices);
	}
}

void i915_guc_hwconfig_free(struct i915_device *i915)
{
	kfree(i915->hwconfig);
	i915->hwconfig = NULL;
	i915->hwconfig_size = 0;
}

static int guc_upload(struct i915_device *i915, struct i915_guc *g, const struct uc_image *img)
{
	guc_prepare_xfer(i915, g);
	/* Xe3P: page faults reported through the main GAM controller's
	 * queues, chosen before the firmware starts */
	if (i915->gt_ip >= I915_IP(20, 0) && guc_main_gamctrl_queues(i915, g))
		guc_write(i915, g, XE3P_MAIN_GAMCTRL_MODE, XE3P_MAIN_GAMCTRL_QUEUE_SELECT);
	guc_xfer_rsa(i915, g, img);
	/* the code at 8 KB into the share; below it the stack */
	int rc = uc_dma(i915, g, g->guc_ggtt, UC_CSS_HEADER_BYTES + img->ucode_size, 0x2000, UOS_MOVE);
	if (rc)
		return rc;
	return guc_wait_ucode(i915, g);
}

/* ---- the command transport ------------------------------------------------------------------------ */

/* Two descriptors of 2 KB, then the host-to-GuC ring and the larger
 * GuC-to-host one.  Messages wrap around the end of a ring. */
#define CTB_DESC_SIZE 2048
#define CT_DESC_H2G 0
#define CT_DESC_G2H CTB_DESC_SIZE
#define CT_H2G_OFF (2 * CTB_DESC_SIZE)
#define CT_H2G_BYTES 4096
#define CT_G2H_OFF (CT_H2G_OFF + CT_H2G_BYTES)
#define CT_G2H_BYTES (4 * CT_H2G_BYTES)
#define CT_TOTAL (CT_G2H_OFF + CT_G2H_BYTES)
#define CTB_MSG_MAX_DWORDS 256

#define CTB_HDR_FENCE(f) ((uint32_t)(f) << 16)
#define CTB_HDR_FENCE_OF(h) ((h) >> 16)
#define CTB_HDR_FORMAT_HXG (0u << 12)
#define CTB_HDR_NUM_DWORDS(h) ((h) & 0xffu)

struct guc_ct_buffer_desc {
	uint32_t head; /* dwords */
	uint32_t tail;
	uint32_t status;
	uint32_t reserved[13];
} __attribute__((packed));

#define GUC_CTB_STATUS_UNUSED (1u << 3)

static volatile struct guc_ct_buffer_desc *ct_desc(struct i915_guc *g, int g2h)
{
	return (volatile struct guc_ct_buffer_desc *)(g->ct_vaddr + (g2h ? CT_DESC_G2H : CT_DESC_H2G));
}

static volatile uint32_t *ct_ring(struct i915_guc *g, int g2h)
{
	return (volatile uint32_t *)(g->ct_vaddr + (g2h ? CT_G2H_OFF : CT_H2G_OFF));
}

static int ct_create(struct i915_device *i915, struct i915_guc *g)
{
	g->ct_obj = guc_alloc(i915, g, CT_TOTAL, &g->ct_ggtt, &g->ct_vaddr);
	if (!g->ct_obj)
		return -ENOMEM;
	g->ct_h2g_size = CT_H2G_BYTES / 4;
	g->ct_g2h_size = CT_G2H_BYTES / 4;
	spinlock_init(&g->ct_lock, "i915_guc_ct");
	spinlock_init(&g->g2h_lock, "i915_guc_g2h");
	return 0;
}

static void ct_flush_ring_span(struct i915_device *i915, struct i915_guc *g, int g2h, uint32_t from,
			       uint32_t n)
{
	volatile uint32_t *ring = ct_ring(g, g2h);
	uint32_t size = g2h ? g->ct_g2h_size : g->ct_h2g_size;
	uint32_t first = n;

	if (from + first > size)
		first = size - from;
	guc_flush(i915, (const void *)&ring[from], first * 4);
	if (n > first)
		guc_flush(i915, (const void *)&ring[0], (n - first) * 4);
}

/* Register both rings (the GuC-to-host one first) and switch the
 * transport on; all of it over the message registers. */
static int ct_enable(struct i915_device *i915, struct i915_guc *g)
{
	int rc;

	mm_memset(g->ct_vaddr, 0, CT_TOTAL);
	clflush_range(g->ct_vaddr, CT_TOTAL);
	g->ct_h2g_tail = 0;
	g->ct_g2h_head = 0;
	g->ct_broken = 0;
	rc = guc_self_cfg(i915, g, KLV_SELF_CFG_G2H_CTB_DESC_ADDR, 2, g->ct_ggtt + CT_DESC_G2H);
	if (!rc)
		rc = guc_self_cfg(i915, g, KLV_SELF_CFG_G2H_CTB_ADDR, 2, g->ct_ggtt + CT_G2H_OFF);
	if (!rc)
		rc = guc_self_cfg(i915, g, KLV_SELF_CFG_G2H_CTB_SIZE, 1, CT_G2H_BYTES);
	if (!rc)
		rc = guc_self_cfg(i915, g, KLV_SELF_CFG_H2G_CTB_DESC_ADDR, 2, g->ct_ggtt + CT_DESC_H2G);
	if (!rc)
		rc = guc_self_cfg(i915, g, KLV_SELF_CFG_H2G_CTB_ADDR, 2, g->ct_ggtt + CT_H2G_OFF);
	if (!rc)
		rc = guc_self_cfg(i915, g, KLV_SELF_CFG_H2G_CTB_SIZE, 1, CT_H2G_BYTES);
	if (rc)
		return rc;
	uint32_t req[2] = { hxg_request(GUC_ACTION_HOST2GUC_CONTROL_CTB), GUC_CTB_CONTROL_ENABLE };
	rc = guc_send_mmio(i915, g, req, 2, NULL, 0);
	if (rc > 0)
		rc = -EPROTO;
	if (rc)
		return rc;
	g->comm = 1;
	return 0;
}

/* A message into the host-to-GuC ring: action[0] the action (and its
 * data0), the rest its data.  A fast request gets no answer unless it
 * fails.  -EBUSY when the ring has no room.  Caller holds ct_lock. */
static int ct_write(struct i915_device *i915, struct i915_guc *g, const uint32_t *action, uint32_t len,
		    int fast, uint16_t *fence_out)
{
	volatile struct guc_ct_buffer_desc *d = ct_desc(g, 0);
	volatile uint32_t *ring = ct_ring(g, 0);
	uint32_t size = g->ct_h2g_size;
	uint32_t tail = g->ct_h2g_tail;

	if (!g->comm || g->ct_broken)
		return -ENODEV;
	guc_flush(i915, (const void *)d, 64);
	uint32_t status = d->status, head = d->head;
	if (status || head >= size) {
		g->ct_broken = 1;
		kprintf("[drm] i915: %s transport broken: head %u, tail %u, status %x\n", guc_name(g),
			head, d->tail, status);
		return -EPIPE;
	}
	uint32_t space = (head - tail - 1 + size) % size;
	if (space < len + 1)
		return -EBUSY;
	uint16_t fence = ++g->ct_fence;
	uint32_t at = tail;
	ring[at] = CTB_HDR_FENCE(fence) | CTB_HDR_FORMAT_HXG | (len & 0xffu);
	at = (at + 1) % size;
	ring[at] = ((fast ? HXG_TYPE_FAST_REQUEST : HXG_TYPE_REQUEST) << HXG_TYPE_SHIFT) |
		   (action[0] & HXG_REQUEST_ACTION_MASK);
	at = (at + 1) % size;
	for (uint32_t i = 1; i < len; i++) {
		ring[at] = action[i];
		at = (at + 1) % size;
	}
	/* the message (and, for a submission, the context's new tail)
	 * visible before the descriptor's tail moves */
	ct_flush_ring_span(i915, g, 0, tail, len + 1);
	g->ct_h2g_tail = at;
	d->tail = at;
	guc_flush(i915, (const void *)d, 64);
	if (fence_out)
		*fence_out = fence;
	guc_notify(i915, g);
	return 0;
}

/* A fast request; caller holds ct_lock. */
static int ct_send_fast(struct i915_device *i915, struct i915_guc *g, const uint32_t *action,
			uint32_t len)
{
	return ct_write(i915, g, action, len, 1, NULL);
}

static void g2h_dispatch(struct i915_device *i915, struct i915_guc *g, const uint32_t *msg, uint32_t n);

/* One message out of the GuC-to-host ring into `msg' (at most `max'
 * dwords kept); the number of dwords it had, 0 when the ring is empty,
 * or an error. */
static int ct_read(struct i915_device *i915, struct i915_guc *g, uint32_t *msg, uint32_t max)
{
	volatile struct guc_ct_buffer_desc *d = ct_desc(g, 1);
	volatile uint32_t *ring = ct_ring(g, 1);
	uint32_t size = g->ct_g2h_size;
	uint32_t head = g->ct_g2h_head;

	if (g->ct_broken)
		return -EPIPE;
	guc_flush(i915, (const void *)d, 64);
	uint32_t status = d->status & ~GUC_CTB_STATUS_UNUSED;
	uint32_t tail = d->tail;
	if (status || tail >= size) {
		g->ct_broken = 1;
		kprintf("[drm] i915: %s transport broken: head %u, tail %u, status %x\n", guc_name(g),
			head, tail, d->status);
		return -EPIPE;
	}
	if (tail == head)
		return 0;
	uint32_t avail = (tail - head + size) % size;
	guc_flush(i915, (const void *)&ring[head], 4);
	uint32_t hdr = ring[head];
	uint32_t len = CTB_HDR_NUM_DWORDS(hdr) + 1;
	if (len > avail) {
		g->ct_broken = 1;
		kprintf("[drm] i915: %s sent an incomplete message (%u of %u dwords)\n", guc_name(g),
			avail, len);
		return -EPIPE;
	}
	ct_flush_ring_span(i915, g, 1, head, len);
	for (uint32_t i = 0; i < len; i++) {
		uint32_t v = ring[(head + i) % size];
		if (i < max)
			msg[i] = v;
	}
	head = (head + len) % size;
	g->ct_g2h_head = head;
	d->head = head;
	guc_flush(i915, (const void *)d, 64);
	return (int)len;
}

/* Everything the GuC has sent, handled outside the ring's lock. */
static void ct_receive(struct i915_device *i915, struct i915_guc *g)
{
	uint32_t msg[CTB_MSG_MAX_DWORDS];
	uint64_t fl;

	if (!g->comm)
		return;
	for (int guard = 0; guard < 1024; guard++) {
		spin_lock_irqsave(&g->g2h_lock, &fl);
		int n = ct_read(i915, g, msg, CTB_MSG_MAX_DWORDS);
		spin_unlock_irqrestore(&g->g2h_lock, fl);
		if (n <= 0)
			return;
		g2h_dispatch(i915, g, msg, n > CTB_MSG_MAX_DWORDS ? CTB_MSG_MAX_DWORDS : (uint32_t)n);
	}
}

/* A request that waits for its answer (thread context, one at a time per
 * GuC): the answer is picked out of the GuC-to-host ring by its fence,
 * the ring polled meanwhile -- the interrupt may not be serviced yet.
 * Returns the response's data, or with `out' the number of data dwords
 * copied there. */
static int ct_send_wait(struct i915_device *i915, struct i915_guc *g, const uint32_t *action,
			uint32_t len, uint32_t *out, uint32_t nout)
{
	uint64_t fl;
	int rc;

	for (int attempt = 0; attempt < 10; attempt++) {
		uint16_t fence = 0;
		for (int t = 0;; t++) {
			spin_lock_irqsave(&g->ct_lock, &fl);
			g->wait_done = 0;
			rc = ct_write(i915, g, action, len, 0, &fence);
			if (!rc)
				g->wait_fence = fence;
			spin_unlock_irqrestore(&g->ct_lock, fl);
			if (rc != -EBUSY || t >= 1000)
				break;
			ct_receive(i915, g);
			lapic_delay_us(100);
		}
		if (rc)
			return rc;
		for (int t = 0; t < 100000 && !g->wait_done; t++) { /* a second */
			ct_receive(i915, g);
			if (!g->wait_done)
				lapic_delay_us(10);
		}
		g->wait_fence = 0xffffffffu;
		if (!g->wait_done) {
			kprintf("[drm] i915: %s did not answer request %04x\n", guc_name(g),
				action[0] & 0xffff);
			return -ETIMEDOUT;
		}
		uint32_t hxg = g->wait_hxg;
		if (HXG_TYPE(hxg) == HXG_TYPE_NO_RESPONSE_RETRY)
			continue;
		if (HXG_TYPE(hxg) == HXG_TYPE_RESPONSE_FAILURE) {
			kprintf("[drm] i915: %s refused request %04x: error %x, hint %u\n", guc_name(g),
				action[0] & 0xffff, HXG_FAILURE_ERROR(hxg), HXG_FAILURE_HINT(hxg));
			return -EIO;
		}
		if (HXG_TYPE(hxg) != HXG_TYPE_RESPONSE_SUCCESS)
			return -EPROTO;
		if (out) {
			uint32_t n = nout < 4 ? nout : 4;
			for (uint32_t i = 0; i < n; i++)
				out[i] = g->wait_data[i];
			return (int)n;
		}
		return (int)HXG_RESPONSE_DATA0(hxg);
	}
	return -EAGAIN;
}

/* The GuC-to-host interrupt, enabled for one GuC.  Gen11 on: one enable
 * register for both GuCs (its upper half), the masks in one register,
 * the primary GuC's in its upper half and the media GuC's in its lower. */
static void guc_irq_enable_one(struct i915_device *i915, struct i915_guc *g)
{
	if (i915->info->gen >= 11) {
		uint32_t bit = g->media ? GUC_INTR_GUC2HOST : GUC_INTR_GUC2HOST << 16;
		i915_write32(i915, GEN11_GUC_SG_INTR_ENABLE, GUC_INTR_GUC2HOST << 16);
		i915_write32(i915, MTL_GUC_MGUC_INTR_MASK,
			     i915_read32(i915, MTL_GUC_MGUC_INTR_MASK) & ~bit);
		return;
	}
	uint32_t bits = GUC_INTR_GUC2HOST << 16;
	i915_write32_fw(i915, GEN8_GT_IIR(2), bits);
	i915_write32_fw(i915, GEN8_GT_IMR(2), i915_read32_fw(i915, GEN8_GT_IMR(2)) & ~bits);
	i915_write32_fw(i915, GEN8_GT_IER(2), i915_read32_fw(i915, GEN8_GT_IER(2)) | bits);
	(void)i915_read32_fw(i915, GEN8_GT_IER(2));
}

void i915_guc_irq_enable(struct i915_device *i915)
{
	if (i915->guc.comm)
		guc_irq_enable_one(i915, &i915->guc);
	if (i915->media_guc.comm)
		guc_irq_enable_one(i915, &i915->media_guc);
}

void i915_guc_irq(struct i915_device *i915)
{
	/* the messages are read by the GT worker, off the interrupt */
	if (i915->gt_worker_ready)
		poll_notify_wq(&i915->gt_wq);
	i915->hangcheck_pending = 1;
}

/* ---- context ids ----------------------------------------------------------------------------------- */

static struct i915_guc_ctx *ctx_slot(struct i915_guc *g, uint32_t id)
{
	if (!g->ctx || id == 0 || id >= I915_GUC_MAX_CONTEXTS)
		return NULL;
	return &g->ctx[id];
}

/* Caller holds ct_lock. */
static int ctx_id_alloc(struct i915_guc *g, struct i915_lrc *lrc)
{
	if (!g->ctx)
		return -1;
	for (uint32_t n = 0; n < I915_GUC_MAX_CONTEXTS; n++) {
		uint32_t id = (g->ctx_next + n) % I915_GUC_MAX_CONTEXTS;
		if (id == 0)
			continue; /* 0 means "none" to the host */
		struct i915_guc_ctx *c = &g->ctx[id];
		if (c->state == GUC_CTX_FREE) {
			mm_memset(c, 0, sizeof(*c));
			c->state = GUC_CTX_LIVE;
			c->lrc = lrc;
			g->ctx_next = id + 1;
			return (int)id;
		}
	}
	return -1;
}

static void ctx_send_deregister(struct i915_device *i915, struct i915_guc *g, uint32_t id,
				struct i915_guc_ctx *c)
{
	uint32_t a[2] = { GUC_ACTION_DEREGISTER_CONTEXT, id };
	if (ct_send_fast(i915, g, a, 2) == 0) {
		c->need_send = GUC_SEND_NONE;
	} else if (c->need_send == GUC_SEND_NONE) {
		c->need_send = GUC_SEND_DEREGISTER;
		g->ctx_pending++;
	} else {
		c->need_send = GUC_SEND_DEREGISTER;
	}
}

static void ctx_send_disable(struct i915_device *i915, struct i915_guc *g, uint32_t id,
			     struct i915_guc_ctx *c)
{
	uint32_t a[3] = { GUC_ACTION_SCHED_CONTEXT_MODE_SET, id, GUC_CONTEXT_DISABLE };
	if (ct_send_fast(i915, g, a, 3) == 0) {
		if (c->need_send != GUC_SEND_NONE)
			g->ctx_pending--;
		c->need_send = GUC_SEND_NONE;
		c->pending_disable = 1;
	} else if (c->need_send == GUC_SEND_NONE) {
		c->need_send = GUC_SEND_DISABLE;
		g->ctx_pending++;
	}
}

/* Messages that did not fit the ring when they were due.  Caller holds
 * ct_lock. */
static void ctx_retry_pending(struct i915_device *i915, struct i915_guc *g)
{
	if (!g->ctx_pending || !g->ctx)
		return;
	for (uint32_t id = 1; id < I915_GUC_MAX_CONTEXTS && g->ctx_pending > 0; id++) {
		struct i915_guc_ctx *c = &g->ctx[id];
		if (c->need_send == GUC_SEND_DISABLE) {
			ctx_send_disable(i915, g, id, c);
		} else if (c->need_send == GUC_SEND_DEREGISTER) {
			uint32_t a[2] = { GUC_ACTION_DEREGISTER_CONTEXT, id };
			if (ct_send_fast(i915, g, a, 2) == 0) {
				c->need_send = GUC_SEND_NONE;
				g->ctx_pending--;
			}
		}
		if (c->need_send != GUC_SEND_NONE)
			break; /* the ring is full: later */
	}
}

/* ---- what the GuC tells the host ---------------------------------------------------------------- */

/* The GuC reset a context that would not be preempted (or hit an error):
 * the requests it had in flight are lost, and fail; the context's ring
 * is emptied in its image so that it starts over with its next request;
 * a context that asked not to be recovered, or hung three times, is
 * banned. */
static void guc_context_reset(struct i915_device *i915, struct i915_guc *g, uint32_t id)
{
	struct i915_lrc *lrc = NULL;
	struct i915_engine *e = NULL;
	uint64_t fl;

	/* the engine read under the lock its release takes */
	spin_lock_irqsave(&g->ct_lock, &fl);
	struct i915_guc_ctx *c = ctx_slot(g, id);
	if (c && c->state == GUC_CTX_LIVE && c->lrc) {
		lrc = c->lrc;
		e = lrc->engine;
	}
	spin_unlock_irqrestore(&g->ct_lock, fl);
	g->resets_seen++;
	if (!lrc || !e) {
		i915_dbg("[drm] i915: %s reset context %u, no longer the host's\n", guc_name(g), id);
		return;
	}
	struct i915_request *lost = NULL, **lp = &lost;

	kprintf("[drm] i915: %s: the %s reset a hung context (id %u)\n", e->name, guc_name(g), id);
	spin_lock_irqsave(&e->lock, &fl);
	/* in flight, and queued behind them: all of it is gone */
	struct i915_request **pp = &e->inflight, *prev = NULL;
	while (*pp) {
		struct i915_request *rq = *pp;
		if (rq->lrc == lrc) {
			*pp = rq->next;
			rq->next = NULL;
			*lp = rq;
			lp = &rq->next;
			continue;
		}
		prev = rq;
		pp = &rq->next;
	}
	e->inflight_tail = prev;
	pp = &e->queue;
	prev = NULL;
	while (*pp) {
		struct i915_request *rq = *pp;
		if (rq->lrc == lrc) {
			*pp = rq->next;
			rq->next = NULL;
			*lp = rq;
			lp = &rq->next;
			continue;
		}
		prev = rq;
		pp = &rq->next;
	}
	e->queue_tail = prev;
	if (e->active_lrc == lrc) {
		e->active_lrc = NULL;
		e->active_ctx = NULL;
		e->active_ctx_id = 0;
	}
	/* The ring starts over after everything written so far.  Only
	 * with requests of the context in hand: they hold the context, and
	 * without them it may be going away on another processor (the
	 * look-up above was under the transport's lock, not this one), its
	 * ring then has nothing left to run anyway. */
	struct i915_lrc_layout l;
	if (lost && lrc->regs && i915_lrc_layout(i915->info->gen_x10, e->class, &l) == 0) {
		lrc->regs[l.ring_head + 1] = lrc->ring.tail;
		lrc->regs[l.ring_tail + 1] = lrc->ring.tail;
		if (l.mi_mode) {
			lrc->regs[l.mi_mode + 1] &= ~(uint32_t)STOP_RING;
			lrc->regs[l.mi_mode + 1] |= (uint32_t)STOP_RING << 16;
		}
		guc_flush(i915, lrc->regs, 4096);
		lrc->ring.head = lrc->ring.tail;
	}
	spin_unlock_irqrestore(&e->lock, fl);

	int first = 1;
	while (lost) {
		struct i915_request *rq = lost;
		lost = rq->next;
		rq->next = NULL;
		if (rq->ctx) {
			rq->ctx->reset_count++;
			if (first) {
				rq->ctx->batch_active++;
				rq->ctx->hang_count++;
				if (rq->ctx->bannable &&
				    (rq->ctx->hang_count >= 3 || !rq->ctx->recoverable))
					rq->ctx->banned = 1;
			} else {
				rq->ctx->batch_pending++;
			}
		}
		rq->guilty = first;
		first = 0;
		if (rq->fence) {
			rq->fence->error = -EIO;
			drm_fence_signal(rq->fence);
		}
		i915_request_put(rq);
	}
	i915->gpu_reset_count++;
	poll_notify_wq(&e->wq);
}

static void g2h_event(struct i915_device *i915, struct i915_guc *g, uint32_t action,
		      const uint32_t *data, uint32_t n)
{
	struct drm_gem_object *drop_img = NULL, *drop_ring = NULL;
	struct i915_vm *drop_vm = NULL;
	struct i915_guc_ctx *c;
	uint64_t fl;

	g->g2h_events++;
	switch (action) {
	case GUC_ACTION_SCHED_CONTEXT_MODE_DONE:
		if (n < 1)
			break;
		spin_lock_irqsave(&g->ct_lock, &fl);
		c = ctx_slot(g, data[0]);
		if (c && c->state != GUC_CTX_FREE) {
			/* the second dword: the state the context is now in */
			int enabled = n >= 2 ? data[1] == GUC_CONTEXT_ENABLE : c->pending_enable;
			if (enabled && c->pending_enable) {
				c->pending_enable = 0;
			} else if (!enabled && c->pending_disable) {
				c->pending_disable = 0;
				if (c->destroy)
					ctx_send_deregister(i915, g, data[0], c);
			} else {
				c->pending_enable = c->pending_disable = 0;
			}
		}
		spin_unlock_irqrestore(&g->ct_lock, fl);
		break;
	case GUC_ACTION_DEREGISTER_CONTEXT_DONE:
		if (n < 1)
			break;
		spin_lock_irqsave(&g->ct_lock, &fl);
		c = ctx_slot(g, data[0]);
		if (c && c->state == GUC_CTX_DYING) {
			drop_img = c->img;
			drop_ring = c->ring;
			drop_vm = c->vm;
			if (c->need_send != GUC_SEND_NONE)
				g->ctx_pending--;
			mm_memset(c, 0, sizeof(*c));
		}
		spin_unlock_irqrestore(&g->ct_lock, fl);
		if (drop_img && !g->dereg_said) {
			g->dereg_said = 1;
			kprintf("[drm] i915: %s let go of its first client context (id %u)\n",
				guc_name(g), data[0]);
		}
		if (drop_img)
			drm_gem_put(drop_img);
		if (drop_ring)
			drm_gem_put(drop_ring);
		if (drop_vm)
			i915_vm_put(drop_vm);
		break;
	case GUC_ACTION_CONTEXT_RESET_NOTIFICATION:
		if (n >= 1)
			guc_context_reset(i915, g, data[0]);
		break;
	case GUC_ACTION_ENGINE_FAILURE_NOTIFICATION:
		g->resets_seen++;
		kprintf("[drm] i915: %s: engine reset failed (class %u, instance %u, reason %08x)\n",
			guc_name(g), n >= 1 ? data[0] : 0, n >= 2 ? data[1] : 0, n >= 3 ? data[2] : 0);
		break;
	case GUC_ACTION_NOTIFY_CRASH_DUMP_POSTED:
	case GUC_ACTION_NOTIFY_EXCEPTION:
		kprintf("[drm] i915: the %s crashed (%s)\n", guc_name(g),
			action == GUC_ACTION_NOTIFY_EXCEPTION ? "exception" : "crash dump posted");
		break;
	case GUC_ACTION_NOTIFY_FLUSH_LOG_BUFFER_TO_FILE:
	case GUC_ACTION_STATE_CAPTURE_NOTIFICATION:
	case GUC_ACTION_TLB_INVALIDATION_DONE:
		/* nobody reads the log or the capture; nothing waits */
		break;
	default:
		if (!g->g2h_unknown++)
			i915_dbg("[drm] i915: %s sent event %04x\n", guc_name(g), action);
		break;
	}
}

static void g2h_dispatch(struct i915_device *i915, struct i915_guc *g, const uint32_t *msg, uint32_t n)
{
	if (n < 2)
		return;
	uint32_t hxg = msg[1];
	uint32_t fence = CTB_HDR_FENCE_OF(msg[0]);

	if (!(hxg & HXG_ORIGIN_GUC)) {
		g->g2h_unknown++;
		return;
	}
	switch (HXG_TYPE(hxg)) {
	case HXG_TYPE_EVENT:
		g2h_event(i915, g, HXG_EVENT_ACTION(hxg), msg + 2, n - 2);
		break;
	case HXG_TYPE_RESPONSE_SUCCESS:
	case HXG_TYPE_RESPONSE_FAILURE:
	case HXG_TYPE_NO_RESPONSE_RETRY:
		if (fence == g->wait_fence && !g->wait_done) {
			for (uint32_t i = 0; i < 4; i++)
				g->wait_data[i] = i + 2 < n ? msg[i + 2] : 0;
			g->wait_hxg = hxg;
			__asm__ volatile("mfence" ::: "memory");
			g->wait_done = 1;
		} else if (HXG_TYPE(hxg) == HXG_TYPE_RESPONSE_FAILURE) {
			/* a fast request the GuC could not carry out */
			if (g->send_failures++ < 8)
				kprintf("[drm] i915: %s failed request %u: error %x, hint %u\n",
					guc_name(g), fence, HXG_FAILURE_ERROR(hxg),
					HXG_FAILURE_HINT(hxg));
		}
		break;
	default:
		g->g2h_unknown++;
		break;
	}
}

void i915_guc_ct_process(struct i915_device *i915)
{
	struct i915_guc *gucs[2] = { &i915->guc, &i915->media_guc };
	uint64_t fl;

	for (int k = 0; k < 2; k++) {
		struct i915_guc *g = gucs[k];
		if (!g->comm)
			continue;
		ct_receive(i915, g);
		if (g->ctx_pending) {
			spin_lock_irqsave(&g->ct_lock, &fl);
			ctx_retry_pending(i915, g);
			spin_unlock_irqrestore(&g->ct_lock, fl);
		}
	}
}

/* ---- submission -------------------------------------------------------------------------------------- */

/* The scheduling policies of a context.  The execution quantum is off
 * (0): the requests of every context write one sequence number per
 * engine, retired in the order they were submitted, so contexts of one
 * engine must complete in that order -- no time-slicing between them.
 * The preemption timeout is the one a reset is judged by: 7.5 s for the
 * render and compute engines, whose threads cannot be preempted midway,
 * 640 ms for the rest. */
static void ctx_policies(const struct i915_engine *e, uint32_t id, uint32_t *p, uint32_t *len,
			 uint32_t quantum_us, int preempt_to_idle)
{
	uint32_t n = 0;
	uint32_t timeout_us = (e->class == I915_ENGINE_CLASS_RENDER ||
			       e->class == I915_ENGINE_CLASS_COMPUTE) ? 7500000 : 640000;
	p[n++] = GUC_ACTION_HOST2GUC_UPDATE_CONTEXT_POLICIES;
	p[n++] = id;
	p[n++] = KLV(KLV_CTX_SCHEDULING_PRIORITY, 1);
	p[n++] = GUC_CLIENT_PRIORITY_KMD_NORMAL;
	p[n++] = KLV(KLV_CTX_EXECUTION_QUANTUM, 1);
	p[n++] = quantum_us;
	p[n++] = KLV(KLV_CTX_PREEMPTION_TIMEOUT, 1);
	p[n++] = timeout_us;
	if (preempt_to_idle) {
		p[n++] = KLV(KLV_CTX_PREEMPT_TO_IDLE_ON_QUANTUM_EXPIRY, 1);
		p[n++] = 1;
	}
	*len = n;
}

/* Register the logical ring and set its policies, then make sure its
 * scheduling is on and the GuC knows its new tail.  Caller holds
 * ct_lock. */
static int lrc_schedule(struct i915_device *i915, struct i915_guc *g, struct i915_engine *e,
			struct i915_lrc *lrc)
{
	struct i915_guc_ctx *c;
	int rc;

	if (!lrc->guc_id) {
		int id = ctx_id_alloc(g, lrc);
		if (id < 0)
			return -ENOSPC;
		lrc->guc_id = (uint32_t)id;
		lrc->guc_registered = 0;
		lrc->guc_enabled = 0;
	}
	c = ctx_slot(g, lrc->guc_id);
	if (!c)
		return -EINVAL;
	if (!lrc->guc_registered) {
		uint32_t lrca = (uint32_t)i915_lrc_descriptor(i915, lrc, e);
		if (e->class == I915_ENGINE_CLASS_RENDER || e->class == I915_ENGINE_CLASS_COMPUTE)
			lrca |= GEN12_CTX_PRIORITY_NORMAL;
		uint32_t reg[12] = {
			GUC_ACTION_REGISTER_CONTEXT,
			CONTEXT_REGISTRATION_FLAG_KMD,
			lrc->guc_id,
			(uint32_t)guc_class_of(e->class),
			1u << guc_logical_instance(i915, g, e->class, e->instance),
			0, 0, 0, 0, 0, /* no work queue: a single logical ring */
			lrca,
			0,
		};
		rc = ct_send_fast(i915, g, reg, 12);
		if (rc)
			return rc;
		lrc->guc_registered = 1;
		c->policy_required = 1;
	}
	if (c->policy_required) {
		uint32_t pol[16], n;
		ctx_policies(e, lrc->guc_id, pol, &n, 0, 0);
		rc = ct_send_fast(i915, g, pol, n);
		if (rc)
			return rc;
		c->policy_required = 0;
	}
	if (!lrc->guc_enabled) {
		uint32_t a[3] = { GUC_ACTION_SCHED_CONTEXT_MODE_SET, lrc->guc_id, GUC_CONTEXT_ENABLE };
		rc = ct_send_fast(i915, g, a, 3);
		if (rc)
			return rc;
		lrc->guc_enabled = 1;
		c->pending_enable = 1;
		return 0;
	}
	uint32_t a[2] = { GUC_ACTION_SCHED_CONTEXT, lrc->guc_id };
	return ct_send_fast(i915, g, a, 2);
}

/* Caller holds e->lock: hand queued requests to the GuC.
 *
 * One logical ring at a time is in flight on an engine.  Every request
 * of an engine writes the engine's one sequence number, and a request
 * counts as complete once that number has passed its own
 * (i915_engine_retire) -- which holds only while the engine completes
 * requests in the order they were numbered.  The GuC does not keep that
 * order between contexts: a context it is running and whose tail moves
 * on is carried on with (a lite restore) ahead of another context
 * submitted in between.  The later number lands first, the request of
 * the other context counts as done before it has run, its objects are
 * released, and the engine then reads and writes pages that belong to
 * somebody else.  So requests of another logical ring wait here until
 * the engine's in-flight requests have retired (which submits again);
 * requests of the logical ring in flight follow it at once.  A full
 * transport leaves the rest queued; the GT worker calls again. */
void i915_guc_submit(struct i915_engine *e)
{
	struct i915_device *i915 = e->i915;
	struct i915_guc *g = guc_of_engine(i915, e);
	uint64_t fl;

	if (!g->comm || !g->ctx) {
		if (e->queue && !(g->send_failures++))
			kprintf("[drm] i915: %s: no %s to submit to\n", e->name, guc_name(g));
		return;
	}
	while (e->queue) {
		struct i915_request *rq = e->queue;
		struct i915_lrc *lrc = rq->lrc;
		if (!lrc || !lrc->obj)
			return;
		if (e->inflight && e->inflight_tail->lrc != lrc)
			return; /* after the logical ring in flight */
		uint32_t tail = rq->ring_tail;
		struct i915_request *last = rq;
		while (last->next && last->next->lrc == lrc) {
			last = last->next;
			tail = last->ring_tail;
		}
		/* the new tail into the image, where the GuC reads it */
		struct i915_lrc_layout l;
		i915_lrc_layout(i915->info->gen_x10, e->class, &l);
		lrc->regs[l.ring_tail + 1] = tail;
		if (!(i915->info->flags & I915_INFO_HAS_LLC))
			clflush_range(lrc->regs, 4096);
		__asm__ volatile("mfence" ::: "memory");

		int was_registered = lrc->guc_registered;
		spin_lock_irqsave(&g->ct_lock, &fl);
		int rc = lrc_schedule(i915, g, e, lrc);
		spin_unlock_irqrestore(&g->ct_lock, fl);
		if (rc) {
			if (rc == -EBUSY) {
				if (!g->ct_full_said) {
					g->ct_full_said = 1;
					kprintf("[drm] i915: %s: the %s's command ring is full; submission waits\n",
						e->name, guc_name(g));
				}
			} else if (!(g->send_failures++)) {
				kprintf("[drm] i915: %s: GuC submission failed (%d)\n", e->name, rc);
			}
			return; /* stays queued */
		}
		if (!was_registered && lrc->guc_registered && rq->ctx && rq->ctx->fp &&
		    !g->reg_said) {
			g->reg_said = 1;
			kprintf("[drm] i915: %s: first client context registered with the %s (id %u)\n",
				e->name, guc_name(g), lrc->guc_id);
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

/* A logical ring going away.  Its scheduling is switched off and, once
 * the GuC says the engine has let go, its registration dropped; until
 * the GuC confirms that, the image and the ring stay referenced, since
 * the engine may still save the context into them.  The id is reused
 * after the confirmation. */
void i915_guc_lrc_release(struct i915_device *i915, struct i915_lrc *lrc, struct i915_vm *vm)
{
	uint64_t fl;

	if (!lrc->guc_id || !lrc->engine)
		return;
	struct i915_guc *g = guc_of_engine(i915, lrc->engine);
	uint32_t id = lrc->guc_id;
	int first = 0;
	spin_lock_irqsave(&g->ct_lock, &fl);
	struct i915_guc_ctx *c = ctx_slot(g, lrc->guc_id);
	if (c && c->state == GUC_CTX_LIVE && c->lrc == lrc) {
		if (lrc->guc_registered && g->comm && !g->rel_said)
			first = g->rel_said = 1;
		c->lrc = NULL;
		if (!lrc->guc_registered || !g->comm) {
			mm_memset(c, 0, sizeof(*c));
		} else {
			c->state = GUC_CTX_DYING;
			c->destroy = 1;
			if (lrc->obj) {
				drm_gem_get(lrc->obj);
				c->img = lrc->obj;
			}
			if (lrc->ring.obj) {
				drm_gem_get(lrc->ring.obj);
				c->ring = lrc->ring.obj;
			}
			if (vm) {
				i915_vm_get(vm);
				c->vm = vm;
			}
			if (lrc->guc_enabled || c->pending_enable)
				ctx_send_disable(i915, g, lrc->guc_id, c);
			else if (!c->pending_disable)
				ctx_send_deregister(i915, g, lrc->guc_id, c);
			/* else: the deregistration follows the disable */
		}
	}
	lrc->guc_id = 0;
	lrc->guc_registered = 0;
	lrc->guc_enabled = 0;
	spin_unlock_irqrestore(&g->ct_lock, fl);
	if (first)
		kprintf("[drm] i915: %s: first context released; the %s is asked to deregister id %u\n",
			lrc->engine->name, guc_name(g), id);
}

int i915_guc_lrc_quiesce(struct i915_device *i915, struct i915_lrc *lrc)
{
	uint64_t fl;
	int sent = 0;

	if (!lrc->guc_id || !lrc->engine)
		return 0;
	struct i915_guc *g = guc_of_engine(i915, lrc->engine);
	if (!g->comm)
		return -ENODEV;
	uint32_t id = lrc->guc_id;
	for (int t = 0; t < 2000 && !sent; t++) {
		spin_lock_irqsave(&g->ct_lock, &fl);
		struct i915_guc_ctx *c = ctx_slot(g, id);
		if (!c || c->state != GUC_CTX_LIVE || !lrc->guc_registered) {
			spin_unlock_irqrestore(&g->ct_lock, fl);
			return 0;
		}
		if (!lrc->guc_enabled && !c->pending_enable && !c->pending_disable) {
			spin_unlock_irqrestore(&g->ct_lock, fl);
			return 0; /* off already */
		}
		uint32_t a[3] = { GUC_ACTION_SCHED_CONTEXT_MODE_SET, id, GUC_CONTEXT_DISABLE };
		if (ct_send_fast(i915, g, a, 3) == 0) {
			c->pending_disable = 1;
			lrc->guc_enabled = 0;
			sent = 1;
		}
		spin_unlock_irqrestore(&g->ct_lock, fl);
		if (!sent) {
			ct_receive(i915, g);
			lapic_delay_us(100);
		}
	}
	if (!sent)
		return -ETIMEDOUT;
	for (int t = 0; t < 20000; t++) { /* 200 ms */
		ct_receive(i915, g);
		spin_lock_irqsave(&g->ct_lock, &fl);
		struct i915_guc_ctx *c = ctx_slot(g, id);
		int done = !c || c->state != GUC_CTX_LIVE || (!c->pending_disable && !c->pending_enable);
		spin_unlock_irqrestore(&g->ct_lock, fl);
		if (done)
			return 0;
		lapic_delay_us(10);
	}
	kprintf("[drm] i915: %s: the %s did not switch context %u out\n", lrc->engine->name,
		guc_name(g), id);
	return -ETIMEDOUT;
}

int i915_guc_engine_hang(struct i915_engine *e)
{
	struct i915_device *i915 = e->i915;
	struct i915_guc *g = guc_of_engine(i915, e);
	uint64_t fl;
	int rc = -ENOENT;

	if (!g->comm || !i915->guc.submission)
		return -ENODEV;
	spin_lock_irqsave(&e->lock, &fl);
	struct i915_lrc *lrc = e->inflight ? e->inflight->lrc : NULL;
	if (lrc && lrc->guc_id && lrc->guc_registered) {
		uint32_t pol[16], n;
		/* a quantum that, once expired, preempts to idle: the GuC
		 * resets the context when it will not yield */
		ctx_policies(e, lrc->guc_id, pol, &n, 1000, 1);
		uint64_t fl2;
		spin_lock_irqsave(&g->ct_lock, &fl2);
		rc = ct_send_fast(i915, g, pol, n);
		spin_unlock_irqrestore(&g->ct_lock, fl2);
	}
	spin_unlock_irqrestore(&e->lock, fl);
	return rc;
}

/* ---- SLPC: the GuC runs the GT's frequency -------------------------------------------------------- */

static uint32_t freq_to_mhz(uint32_t units)
{
	return (units * 50 + 1) / 3; /* units of 50/3 MHz */
}

/* The GT's frequency range, in 50/3 MHz units: RP0 the highest, RPe the
 * efficient one, RPn the lowest. */
static void guc_freq_caps(struct i915_device *i915, const struct i915_guc *g, uint32_t *rp0,
			  uint32_t *rpe, uint32_t *rpn)
{
	if (i915->gt_ip >= I915_IP(12, 70)) {
		uint32_t cap = i915_read32(i915, g->media ? MTL_MEDIAP_STATE_CAP : MTL_RP_STATE_CAP);
		uint32_t e = i915_read32(i915, g->media ? MTL_MPE_FREQUENCY : MTL_GT_RPE_FREQUENCY);
		*rp0 = cap & MTL_RP0_CAP_MASK;
		*rpn = (cap & MTL_RPN_CAP_MASK) >> MTL_RPN_CAP_SHIFT;
		*rpe = e & MTL_RPE_MASK;
		return;
	}
	uint32_t cap = i915_read32(i915, GEN6_RP_STATE_CAP);
	*rp0 = (cap & 0xff) * 3;
	*rpn = ((cap >> 16) & 0xff) * 3;
	*rpe = ((i915_read32(i915, GEN10_FREQ_INFO_REC) & RPE_MASK) >> 8) * 3;
}

static int slpc_set_param(struct i915_device *i915, struct i915_guc *g, uint32_t id, uint32_t value)
{
	uint32_t req[4] = { GUC_ACTION_HOST2GUC_PC_SLPC_REQUEST, SLPC_EVENT(SLPC_EVENT_PARAMETER_SET, 2),
			    id, value };
	int rc = ct_send_wait(i915, g, req, 4, NULL, 0);
	return rc > 0 ? -EPROTO : rc;
}

static void slpc_mem_set_param(struct slpc_shared_data_head *d, uint32_t id, uint32_t value)
{
	d->override_bits[id >> 5] |= 1u << (id % 32);
	d->override_values[id] = value;
}

/* The shared data reset (only the performance task on; the balancer and
 * the duty-cycle control off before 12.70), the reset event, then the
 * limits: the maximum at RP0, the minimum at the efficient frequency --
 * the GT runs between the two as the load asks. */
static int slpc_enable(struct i915_device *i915, struct i915_guc *g)
{
	struct slpc_shared_data_head *d = (struct slpc_shared_data_head *)g->slpc_vaddr;
	int rc;

	if (!g->slpc_obj)
		return -ENODEV;
	mm_memset(g->slpc_vaddr, 0, SLPC_SHARED_DATA_BYTES);
	d->size = SLPC_SHARED_DATA_BYTES;
	slpc_mem_set_param(d, SLPC_PARAM_TASK_ENABLE_GTPERF, 1);
	slpc_mem_set_param(d, SLPC_PARAM_TASK_DISABLE_GTPERF, 0);
	if (i915->gt_ip < I915_IP(12, 70)) {
		slpc_mem_set_param(d, SLPC_PARAM_TASK_DISABLE_BALANCER, 1);
		slpc_mem_set_param(d, SLPC_PARAM_TASK_ENABLE_BALANCER, 0);
		slpc_mem_set_param(d, SLPC_PARAM_TASK_DISABLE_DCC, 1);
		slpc_mem_set_param(d, SLPC_PARAM_TASK_ENABLE_DCC, 0);
	}
	clflush_range(g->slpc_vaddr, SLPC_SHARED_DATA_BYTES);

	uint32_t reset[4] = { GUC_ACTION_HOST2GUC_PC_SLPC_REQUEST, SLPC_EVENT(SLPC_EVENT_RESET, 2),
			      g->slpc_ggtt, 0 };
	rc = ct_send_wait(i915, g, reset, 4, NULL, 0);
	if (rc) {
		kprintf("[drm] i915: %s SLPC reset failed (%d)\n", guc_name(g), rc > 0 ? -EPROTO : rc);
		return rc > 0 ? -EPROTO : rc;
	}
	int running = 0;
	for (int t = 0; t < 5000 && !running; t++) { /* 50 ms */
		clflush_range(g->slpc_vaddr, 64);
		running = ((volatile struct slpc_shared_data_head *)d)->global_state ==
			  SLPC_GLOBAL_STATE_RUNNING;
		if (!running)
			lapic_delay_us(10);
	}
	if (!running) {
		clflush_range(g->slpc_vaddr, 64);
		kprintf("[drm] i915: %s SLPC did not start (state %u)\n", guc_name(g),
			((volatile struct slpc_shared_data_head *)d)->global_state);
		return -EIO;
	}
	uint32_t query[4] = { GUC_ACTION_HOST2GUC_PC_SLPC_REQUEST,
			      SLPC_EVENT(SLPC_EVENT_QUERY_TASK_STATE, 2), g->slpc_ggtt, 0 };
	(void)ct_send_wait(i915, g, query, 4, NULL, 0);
	clflush_range(g->slpc_vaddr, 4096);

	/* the GuC receives the ARAT timer's expiry */
	guc_write(i915, g, GEN6_PMINTRMSK, guc_read(i915, g, GEN6_PMINTRMSK) & ~ARAT_EXPIRED_INTRMSK);

	uint32_t rp0, rpe, rpn;
	guc_freq_caps(i915, g, &rp0, &rpe, &rpn);
	g->rp0_mhz = freq_to_mhz(rp0);
	g->rpe_mhz = freq_to_mhz(rpe);
	g->rpn_mhz = freq_to_mhz(rpn);
	if (!g->rp0_mhz || g->rpe_mhz > g->rp0_mhz || g->rpe_mhz < g->rpn_mhz)
		g->rpe_mhz = g->rpn_mhz;
	rc = slpc_set_param(i915, g, SLPC_PARAM_GLOBAL_MAX_GT_UNSLICE_FREQ_MHZ, g->rp0_mhz);
	if (rc) {
		kprintf("[drm] i915: %s SLPC: the maximum frequency was refused (%d)\n", guc_name(g), rc);
		return rc;
	}
	(void)slpc_set_param(i915, g, SLPC_PARAM_IGNORE_EFFICIENT_FREQUENCY, 0);
	/* Panther Lake (and Wildcat Lake, a Panther Lake): the duty cycle
	 * control off */
	if (i915->info->platform == I915_PLATFORM_PANTHERLAKE ||
	    i915->info->platform == I915_PLATFORM_WILDCATLAKE) {
		if (slpc_set_param(i915, g, SLPC_PARAM_TASK_ENABLE_DCC, 0) == 0)
			(void)slpc_set_param(i915, g, SLPC_PARAM_TASK_DISABLE_DCC, 1);
	}
	/* Battlemage's GT (Wa_14022085890: G21's media GT as well) runs at
	 * 1200 MHz at least */
	if (((i915->info->platform == I915_PLATFORM_BATTLEMAGE && !g->media) ||
	     i915->subplatform == I915_SUBPLATFORM_BMG_G21) &&
	    g->rpe_mhz < 1200 && g->rp0_mhz >= 1200)
		g->rpe_mhz = 1200;
	rc = slpc_set_param(i915, g, SLPC_PARAM_GLOBAL_MIN_GT_UNSLICE_FREQ_MHZ, g->rpe_mhz);
	if (rc)
		i915_dbg("[drm] i915: %s SLPC: minimum frequency not set (%d)\n", guc_name(g), rc);
	if (i915->info->platform == I915_PLATFORM_DG2)
		(void)slpc_set_param(i915, g, SLPC_PARAM_MEDIA_FF_RATIO_MODE,
				     SLPC_MEDIA_RATIO_MODE_DYNAMIC_CONTROL);
	if (slpc_set_param(i915, g, SLPC_PARAM_STRATEGIES, SLPC_OPTIMIZED_STRATEGY_COMPUTE))
		i915_dbg("[drm] i915: %s SLPC: no compute strategy\n", guc_name(g));
	if (slpc_set_param(i915, g, SLPC_PARAM_POWER_PROFILE, SLPC_POWER_PROFILES_BASE))
		i915_dbg("[drm] i915: %s SLPC: no power profile\n", guc_name(g));
	g->slpc = 1;
	kprintf("[drm] i915: %s SLPC: %s clock %u-%u MHz (efficient to maximum; lowest %u MHz)\n",
		guc_name(g), g->media ? "media" : "GT", g->rpe_mhz, g->rp0_mhz, g->rpn_mhz);
	return 0;
}

/* ---- submission on --------------------------------------------------------------------------------- */

static int guc_submission_start(struct i915_device *i915, struct i915_guc *g)
{
	int rc;

	if (!g->loaded || !g->comm || !g->ctx)
		return -ENODEV;
	/* semaphore signals to the GuC, which owns the scheduling */
	if (i915->info->gen >= 12)
		guc_write(i915, g, GEN12_GUC_SEM_INTR_ENABLES,
			  GUC_SEM_INTR_ROUTE_TO_GUC | GUC_SEM_INTR_ENABLE_ALL);

	/* the engines' busyness records, kept by the GuC in the ADS */
	struct guc_ads_blob *b = (struct guc_ads_blob *)g->ads_vaddr;
	for (unsigned d = 0; d < GUC_NUM_ENGINE_DESCS; d++) {
		const struct guc_engine_desc *ed = &guc_engines[d];
		if (!(g->engine_mask & ed->bit))
			continue;
		struct guc_engine_usage_record *r =
			&b->engine_usage[guc_class_of(ed->class)][ed->instance];
		r->current_context_index = 0xffffffffu;
		r->last_switch_in_stamp = 0;
		r->total_runtime = 0;
	}
	clflush_range(b->engine_usage, sizeof(b->engine_usage));
	uint32_t util[3] = { GUC_ACTION_SET_ENG_UTIL_BUFF,
			     g->ads_ggtt + (uint32_t)__builtin_offsetof(struct guc_ads_blob, engine_usage),
			     0 };
	rc = ct_send_wait(i915, g, util, 3, NULL, 0);
	if (rc < 0) {
		kprintf("[drm] i915: %s did not take the engine usage buffer (%d)\n", guc_name(g), rc);
		return rc;
	}

	/* the render/compute yield, from interface 1.1.0 on */
	if (g->submit_version >= GUC_VER(1, 1, 0)) {
		uint32_t pol[4] = { GUC_ACTION_UPDATE_SCHEDULING_POLICIES_KLV,
				    KLV(KLV_SCHED_RENDER_COMPUTE_YIELD, 2),
				    GLOBAL_SCHEDULE_POLICY_RC_YIELD_DURATION,
				    GLOBAL_SCHEDULE_POLICY_RC_YIELD_RATIO };
		rc = ct_send_wait(i915, g, pol, 4, NULL, 0);
		if (rc < 0) {
			kprintf("[drm] i915: %s refused the global scheduling policies (%d)\n",
				guc_name(g), rc);
			return rc;
		}
		if (rc != 1)
			i915_dbg("[drm] i915: %s took %d of 1 scheduling policies\n", guc_name(g), rc);
	}
	if (guc_wants_slpc(i915))
		(void)slpc_enable(i915, g);
	return 0;
}

int i915_guc_submission_enable(struct i915_device *i915)
{
	struct i915_guc *g = &i915->guc, *m = &i915->media_guc;
	int rc;

	if (!g->loaded || !g->comm)
		return -ENODEV;
	rc = guc_submission_start(i915, g);
	if (rc)
		return rc;
	if (m->loaded) {
		rc = guc_submission_start(i915, m);
		if (rc) {
			kprintf("[drm] i915: media GuC did not accept submission (%d)\n", rc);
			return rc;
		}
	}
	g->submission = 1;
	if (m->loaded)
		m->submission = 1;
	/* from now on every change of the global space reaches the GuCs */
	i915_guc_ggtt_invalidate(i915);
	kprintf("[drm] i915: submission through the GuC%s\n",
		m->loaded ? " (render, copy and compute) and the media GuC (video)" : "");
	return 0;
}

/* Xe2 on: a GuC that schedules is asked to invalidate its translations
 * itself (a fast request; the GuC confirms with an event nobody waits
 * for); before that, and where the transport is full, the descriptor
 * registers do it. */
static void guc_ggtt_invalidate_xe2(struct i915_device *i915, struct i915_guc *g)
{
	uint64_t fl;
	int rc = -ENODEV;

	if (g->comm && g->submission) {
		uint32_t a[3] = { GUC_ACTION_TLB_INVALIDATION, 0,
				  GUC_TLB_INVAL_TYPE_GUC | GUC_TLB_INVAL_MODE_HEAVY |
					  GUC_TLB_INVAL_FLUSH_CACHE };
		spin_lock_irqsave(&g->ct_lock, &fl);
		if (!++g->tlb_seqno)
			g->tlb_seqno = 1;
		a[1] = g->tlb_seqno;
		rc = ct_send_fast(i915, g, a, 3);
		spin_unlock_irqrestore(&g->ct_lock, fl);
	}
	if (rc) {
		i915_write32(i915, guc_reg(g, XE2_GUC_TLB_INV_DESC1), XE2_GUC_TLB_INV_DESC1_INVALIDATE);
		i915_write32(i915, guc_reg(g, XE2_GUC_TLB_INV_DESC0), XE2_GUC_TLB_INV_DESC0_VALID);
	}
}

void i915_guc_ggtt_invalidate(struct i915_device *i915)
{
	struct i915_guc *gucs[2] = { &i915->guc, &i915->media_guc };

	for (int k = 0; k < 2; k++) {
		struct i915_guc *g = gucs[k];
		if (!g->loaded && !g->wanted)
			continue;
		if (i915->gt_ip >= I915_IP(20, 0))
			guc_ggtt_invalidate_xe2(i915, g);
		else if (i915->info->gen >= 12)
			i915_write32(i915, guc_reg(g, GEN12_GUC_TLB_INV_CR),
				     GEN12_GUC_TLB_INV_CR_INVALIDATE);
		else
			i915_write32(i915, GEN8_GTCR, GEN8_GTCR_INVALIDATE);
	}
}

/* ---- the HuC ------------------------------------------------------------------------------------------- */

static int huc_auth(struct i915_device *i915, struct i915_guc *g)
{
	uint32_t req[2] = { GUC_ACTION_AUTHENTICATE_HUC, g->huc_rsa_ggtt };
	int rc = ct_send_wait(i915, g, req, 2, NULL, 0);
	if (rc < 0) {
		kprintf("[drm] i915: HuC authentication request failed (%d)\n", rc);
		return rc;
	}
	uint32_t reg = i915->info->gen >= 11 ? GEN11_HUC_KERNEL_LOAD_INFO : HUC_STATUS2;
	uint32_t bit = i915->info->gen >= 11 ? HUC_LOAD_SUCCESSFUL : HUC_FW_VERIFIED;
	for (int t = 0; t < 5000; t++) { /* 50 ms */
		if (guc_read(i915, g, reg) & bit) {
			g->huc_authenticated = 1;
			return 0;
		}
		lapic_delay_us(10);
	}
	kprintf("[drm] i915: HuC authentication did not complete (%08x)\n", guc_read(i915, g, reg));
	return -ETIMEDOUT;
}

/* ---- policy and bring-up ------------------------------------------------------------------------------- */

/* Which parts take the firmware: those that cannot submit without it;
 * Ice Lake and its relatives, as before; and of the Gen12 parts those the
 * platform's own policy runs it on -- Alder Lake-S for the HuC only,
 * Alder Lake-P, Raptor Lake and DG1 with the HuC as well.  Submission
 * stays with the execution lists on all of them.  Tiger Lake and Rocket
 * Lake are not meant to run it, and Gen9 keeps the execution lists
 * alone -- the one platform this driver has been run on. */
int i915_guc_wants_load(struct i915_device *i915)
{
	const struct intel_device_info *info = i915->info;

	if (!(info->flags & I915_INFO_HAS_GUC) || !info->guc_fw)
		return 0;
	if (info->flags & I915_INFO_GUC_MANDATORY)
		return 1;
	if (info->gen == 11)
		return 1;
	if (info->gen_x10 >= 120)
		return info->platform != I915_PLATFORM_TIGERLAKE &&
		       info->platform != I915_PLATFORM_ROCKETLAKE;
	return 0;
}

/* Every object of one GuC freed; the image objects only where this GuC
 * owns them (the media GuC uses the primary's). */
static void guc_release_objects(struct i915_guc *g)
{
	struct drm_gem_object **objs[] = { &g->ct_obj, &g->ads_obj, &g->log_obj, &g->slpc_obj,
					   &g->huc_rsa_obj, &g->huc_obj, &g->guc_rsa_obj, &g->guc_obj };
	for (unsigned i = 0; i < sizeof(objs) / sizeof(objs[0]); i++) {
		if (*objs[i])
			drm_gem_put(*objs[i]);
		*objs[i] = NULL;
	}
	if (g->ctx) {
		for (uint32_t id = 1; id < I915_GUC_MAX_CONTEXTS; id++) {
			struct i915_guc_ctx *c = &g->ctx[id];
			if (c->img)
				drm_gem_put(c->img);
			if (c->ring)
				drm_gem_put(c->ring);
			if (c->vm)
				i915_vm_put(c->vm);
		}
		kfree(g->ctx);
		g->ctx = NULL;
	}
	kfree(g->regset);
	g->regset = NULL;
	g->ct_vaddr = g->ads_vaddr = g->slpc_vaddr = NULL;
}

static void guc_setup(struct i915_device *i915, struct i915_guc *g, int media)
{
	g->media = media;
	g->gsi = media ? I915_MEDIA_GT_BASE : 0;
	if (i915->info->gen >= 11) {
		g->send_base = media ? MEDIA_SOFT_SCRATCH(0) : GEN11_SOFT_SCRATCH(0);
		g->send_count = GEN11_SOFT_SCRATCH_COUNT;
		g->notify_reg = media ? MEDIA_GUC_HOST_INTERRUPT : GEN11_GUC_HOST_INTERRUPT;
	} else {
		g->send_base = SOFT_SCRATCH(0);
		g->send_count = 4;
		g->notify_reg = GUC_SEND_INTERRUPT;
	}
	g->wait_fence = 0xffffffffu;
}

/* Everything one GuC needs, its image loaded and started, its transport
 * opened.  The primary GuC also uploads (and has authenticated) the HuC
 * where the host loads it. */
static int guc_bringup(struct i915_device *i915, struct i915_guc *g, const struct uc_image *img,
		       const struct uc_image *huc, uint32_t fused)
{
	int sub = guc_wants_submission(i915);
	int rc;

	g->fw_version = img->version;
	g->submit_version = guc_submit_version(img->version, img->vf_version);
	g->guc_ucode_size = img->ucode_size;
	g->guc_rsa_size = img->rsa_size;
	g->private_data_size = img->private_data_size;
	g->engine_mask = !i915->has_media_gt ? fused :
			 g->media ? (fused & VIDEO_ENGINES) : (fused & ~VIDEO_ENGINES);
	if (!g->media && i915->has_media_gt)
		g->vdbox_sfc_access = 0;
	if (huc) {
		g->huc_version = huc->version;
		g->huc_ucode_size = huc->ucode_size;
		g->huc_rsa_size = huc->rsa_size;
	}
	rc = wopcm_init(i915, g, UC_CSS_HEADER_BYTES + img->ucode_size,
			huc ? UC_CSS_HEADER_BYTES + huc->ucode_size : 0);
	if (rc)
		return rc;
	if (g->media) {
		/* the primary GuC's copy of the same image */
		g->guc_ggtt = i915->guc.guc_ggtt;
		g->guc_rsa_ggtt = i915->guc.guc_rsa_ggtt;
	} else {
		g->guc_obj = image_to_ggtt(i915, img->data, img->len, &g->guc_ggtt);
		if (!g->guc_obj)
			return -ENOMEM;
		/* a signature larger than the scratch registers is read
		 * from memory the GuC reaches */
		if (img->rsa_size > UOS_RSA_SCRATCH_COUNT * 4) {
			g->guc_rsa_obj = image_to_ggtt(i915,
						       (const uint8_t *)img->data + UC_CSS_HEADER_BYTES +
							       img->ucode_size,
						       img->rsa_size, &g->guc_rsa_ggtt);
			if (!g->guc_rsa_obj)
				return -ENOMEM;
		}
	}
	if (huc) {
		g->huc_obj = image_to_ggtt(i915, huc->data, huc->len, &g->huc_ggtt);
		g->huc_rsa_obj = image_to_ggtt(i915,
					       (const uint8_t *)huc->data + UC_CSS_HEADER_BYTES +
						       huc->ucode_size,
					       huc->rsa_size, &g->huc_rsa_ggtt);
		if (!g->huc_obj || !g->huc_rsa_obj)
			return -ENOMEM;
	}
	rc = log_create(i915, g);
	if (rc)
		return rc;
	rc = ads_create(i915, g);
	if (rc)
		return rc;
	rc = ct_create(i915, g);
	if (rc)
		return rc;
	if (guc_wants_slpc(i915)) {
		g->slpc_obj = guc_alloc(i915, g, SLPC_SHARED_DATA_BYTES, &g->slpc_ggtt, &g->slpc_vaddr);
		if (!g->slpc_obj)
			return -ENOMEM;
	}
	if (sub) {
		g->ctx = kcalloc(I915_GUC_MAX_CONTEXTS, sizeof(*g->ctx));
		if (!g->ctx)
			return -ENOMEM;
		g->ctx_next = 1;
	}
	i915_guc_ggtt_invalidate(i915);

	/* Gen9 wants up to three tries at the load; one is the norm. */
	int attempts = i915->info->gen == 9 ? 3 : 1;
	rc = -EIO;
	while (attempts-- > 0) {
		/* always from reset, so the state and the timing are known */
		if (guc_reset(i915, g))
			continue;
		if (huc && uc_dma(i915, g, g->huc_ggtt, UC_CSS_HEADER_BYTES + huc->ucode_size, 0,
				  HUC_UKERNEL) == 0)
			g->huc_loaded = 1;
		ads_populate(i915, g);
		guc_write_params(i915, g);
		rc = guc_upload(i915, g, img);
		if (rc == 0)
			break;
	}
	if (rc)
		return rc;
	g->loaded = 1;
	if (!g->media)
		guc_hwconfig_fetch(i915, g);
	kprintf("[drm] i915: %s firmware %u.%u.%u running, submission interface %u.%u.%u%s\n",
		guc_name(g), GUC_VER_MAJOR(g->fw_version), GUC_VER_MINOR(g->fw_version),
		GUC_VER_PATCH(g->fw_version), GUC_VER_MAJOR(g->submit_version),
		GUC_VER_MINOR(g->submit_version), GUC_VER_PATCH(g->submit_version),
		sub ? "" : " (submission stays with the execution lists)");
	rc = ct_enable(i915, g);
	if (rc) {
		kprintf("[drm] i915: %s command transport did not open (%d)\n", guc_name(g), rc);
		return rc;
	}
	guc_irq_enable_one(i915, g);
	/* anything sent before the interrupt was on */
	ct_receive(i915, g);
	if (g->huc_loaded && huc_auth(i915, g) == 0)
		kprintf("[drm] i915: HuC firmware %u.%u.%u authenticated\n",
			GUC_VER_MAJOR(g->huc_version), GUC_VER_MINOR(g->huc_version),
			GUC_VER_PATCH(g->huc_version));
	return 0;
}

int i915_guc_init(struct i915_device *i915)
{
	struct i915_guc *g = &i915->guc, *m = &i915->media_guc;
	struct uc_image guc_img, huc_img;
	int have_huc = 0;
	uint8_t sfc = 0;
	int rc;

	if (!i915_guc_wants_load(i915))
		return 0;
	guc_setup(i915, g, 0);
	g->wanted = 1;
	if (uc_image_open(i915, "GuC", i915->info->guc_fw, &guc_img))
		return -ENOENT;
	if (i915->info->huc_fw) {
		/* From 12.55 on the HuC image is the security controller's
		 * to load and authenticate; the host does not touch it. */
		if (i915->gt_ip >= I915_IP(12, 55))
			i915_dbg("[drm] i915: HuC firmware %s is loaded by the GSC; left off\n",
				 i915->info->huc_fw);
		else if (uc_image_open(i915, "HuC", i915->info->huc_fw, &huc_img) == 0)
			have_huc = 1;
	}
	uint32_t fused = guc_fused_engines(i915, &sfc);
	g->vdbox_sfc_access = sfc;
	rc = guc_bringup(i915, g, &guc_img, have_huc ? &huc_img : NULL, fused);
	if (rc) {
		kprintf("[drm] i915: GuC not loaded (%d)%s\n", rc,
			guc_wants_submission(i915) ? "; the GT stays off" : "; execution lists carry on");
		g->comm = 0;
		g->loaded = 0;
		guc_release_objects(g);
		goto out;
	}
	if (i915->has_media_gt && guc_wants_submission(i915)) {
		guc_setup(i915, m, 1);
		m->wanted = 1;
		m->vdbox_sfc_access = sfc;
		int mrc = guc_bringup(i915, m, &guc_img, NULL, fused);
		if (mrc) {
			kprintf("[drm] i915: media GuC not loaded (%d); the video engines stay off\n", mrc);
			m->comm = 0;
			m->loaded = 0;
			m->wanted = 0;
			guc_release_objects(m);
		}
	}
	rc = 0;
out:
	firmware_release(guc_img.data);
	if (have_huc)
		firmware_release(huc_img.data);
	return rc;
}

void i915_guc_fini(struct i915_device *i915)
{
	struct i915_guc *gucs[2] = { &i915->media_guc, &i915->guc };

	i915_guc_hwconfig_free(i915);
	for (int k = 0; k < 2; k++) {
		struct i915_guc *g = gucs[k];
		g->comm = 0;
		g->submission = 0;
		g->slpc = 0;
		g->loaded = 0;
		guc_release_objects(g);
	}
}
