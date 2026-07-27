// LikeOS-64 VMware SVGA II display driver
//
// Device interface definitions derived from the openly documented VMware
// SVGA II virtual display adapter (as implemented by VMware products, QEMU
// "-vga vmware" and VirtualBox "VMSVGA").  All values below are part of the
// stable guest/host ABI of that device.
//
// Driver architecture (DRM/KMS-shaped, so it can later evolve into a full
// display-manager style driver):
//   svga_connector  — the (virtual) monitor: EDID, display topology
//   svga_crtc       — scanout state: current mode, enable/disable
//   svga_fb         — a scanout buffer (front buffer today, surfaces later)
//   svga_bo         — GEM/TTM-like guest memory buffer object (GMR-backed)
//
// Locking model:
//   fifo_lock (spin)    — FIFO reserve/commit/producer index/doorbell
//   reg_lock  (spin)    — index/value register port pairs
//   cursor_lock (spin)  — cursor image + position updates
//   irq_lock  (spin)    — IRQ-visible state (pending flags, fence wakeups)
//   modeset_mutex       — sleeping mutex for mode changes (infrequent)
//   per-object spinlocks + refcounts for buffer objects

#ifndef _KERNEL_DEV_VIDEO_VMSVGA2_H_
#define _KERNEL_DEV_VIDEO_VMSVGA2_H_

#include <kernel/uapi/types.h>
#include <kernel/io/console.h> // framebuffer_info_t

// ---------------------------------------------------------------------------
// PCI identity
// ---------------------------------------------------------------------------
#define SVGA_PCI_VENDOR_ID 0x15AD
#define SVGA_PCI_DEVICE_ID 0x0405

// I/O port offsets from BAR0 (accessed as 32-bit in/out)
#define SVGA_INDEX_PORT 0x0
#define SVGA_VALUE_PORT 0x1
#define SVGA_BIOS_PORT 0x2
#define SVGA_IRQSTATUS_PORT 0x8

// ---------------------------------------------------------------------------
// Device version negotiation
// ---------------------------------------------------------------------------
#define SVGA_MAGIC 0x900000UL
#define SVGA_MAKE_ID(ver) ((uint32_t)((SVGA_MAGIC << 8) | (ver)))
#define SVGA_VERSION_2 2
#define SVGA_ID_2 SVGA_MAKE_ID(2)
#define SVGA_VERSION_1 1
#define SVGA_ID_1 SVGA_MAKE_ID(1)
#define SVGA_VERSION_0 0
#define SVGA_ID_0 SVGA_MAKE_ID(0)
#define SVGA_ID_INVALID 0xFFFFFFFFU

// ---------------------------------------------------------------------------
// Registers (accessed via index/value port pair)
// ---------------------------------------------------------------------------
enum {
	SVGA_REG_ID = 0,
	SVGA_REG_ENABLE = 1,
	SVGA_REG_WIDTH = 2,
	SVGA_REG_HEIGHT = 3,
	SVGA_REG_MAX_WIDTH = 4,
	SVGA_REG_MAX_HEIGHT = 5,
	SVGA_REG_DEPTH = 6,
	SVGA_REG_BITS_PER_PIXEL = 7,
	SVGA_REG_PSEUDOCOLOR = 8,
	SVGA_REG_RED_MASK = 9,
	SVGA_REG_GREEN_MASK = 10,
	SVGA_REG_BLUE_MASK = 11,
	SVGA_REG_BYTES_PER_LINE = 12,
	SVGA_REG_FB_START = 13,
	SVGA_REG_FB_OFFSET = 14,
	SVGA_REG_VRAM_SIZE = 15,
	SVGA_REG_FB_SIZE = 16,
	SVGA_REG_CAPABILITIES = 17,
	SVGA_REG_MEM_START = 18, // FIFO start (physical)
	SVGA_REG_MEM_SIZE = 19, // FIFO size
	SVGA_REG_CONFIG_DONE = 20, // set 1 after FIFO initialized
	SVGA_REG_SYNC = 21, // write 1 to force device processing
	SVGA_REG_BUSY = 22, // read: 1 while device is processing
	SVGA_REG_GUEST_ID = 23,
	SVGA_REG_CURSOR_ID = 24,
	SVGA_REG_CURSOR_X = 25,
	SVGA_REG_CURSOR_Y = 26,
	SVGA_REG_CURSOR_ON = 27,
	SVGA_REG_HOST_BITS_PER_PIXEL = 28,
	SVGA_REG_SCRATCH_SIZE = 29,
	SVGA_REG_MEM_REGS = 30,
	SVGA_REG_NUM_DISPLAYS = 31,
	SVGA_REG_PITCHLOCK = 32,
	SVGA_REG_IRQMASK = 33,
	SVGA_REG_NUM_GUEST_DISPLAYS = 34,
	SVGA_REG_DISPLAY_ID = 35,
	SVGA_REG_DISPLAY_IS_PRIMARY = 36,
	SVGA_REG_DISPLAY_POSITION_X = 37,
	SVGA_REG_DISPLAY_POSITION_Y = 38,
	SVGA_REG_DISPLAY_WIDTH = 39,
	SVGA_REG_DISPLAY_HEIGHT = 40,
	SVGA_REG_GMR_ID = 41,
	SVGA_REG_GMR_DESCRIPTOR = 42,
	SVGA_REG_GMR_MAX_IDS = 43,
	SVGA_REG_GMR_MAX_DESCRIPTOR_LENGTH = 44,
	SVGA_REG_TRACES = 45,
	SVGA_REG_GMRS_MAX_PAGES = 46,
	SVGA_REG_MEMORY_SIZE = 47,
	SVGA_REG_TOP = 48,
};

#define SVGA_GUEST_ID_OTHER 0x500A // generic guest OS identifier

// SVGA_REG_CAPABILITIES bits
#define SVGA_CAP_NONE 0x00000000
#define SVGA_CAP_RECT_COPY 0x00000002
#define SVGA_CAP_CURSOR 0x00000020
#define SVGA_CAP_CURSOR_BYPASS 0x00000040
#define SVGA_CAP_CURSOR_BYPASS_2 0x00000080
#define SVGA_CAP_8BIT_EMULATION 0x00000100
#define SVGA_CAP_ALPHA_CURSOR 0x00000200
#define SVGA_CAP_3D 0x00004000
#define SVGA_CAP_EXTENDED_FIFO 0x00008000
#define SVGA_CAP_MULTIMON 0x00010000
#define SVGA_CAP_PITCHLOCK 0x00020000
#define SVGA_CAP_IRQMASK 0x00040000
#define SVGA_CAP_DISPLAY_TOPOLOGY 0x00080000
#define SVGA_CAP_GMR 0x00100000
#define SVGA_CAP_TRACES 0x00200000
#define SVGA_CAP_GMR2 0x00400000
#define SVGA_CAP_SCREEN_OBJECT_2 0x00800000

// SVGA_REG_IRQMASK / IRQSTATUS bits
#define SVGA_IRQFLAG_ANY_FENCE 0x1
#define SVGA_IRQFLAG_FIFO_PROGRESS 0x2
#define SVGA_IRQFLAG_FENCE_GOAL 0x4

// ---------------------------------------------------------------------------
// FIFO (BAR2) — 32-bit word indices
// ---------------------------------------------------------------------------
enum {
	SVGA_FIFO_MIN = 0,
	SVGA_FIFO_MAX = 1,
	SVGA_FIFO_NEXT_CMD = 2,
	SVGA_FIFO_STOP = 3,
	// Extended FIFO registers (valid when SVGA_CAP_EXTENDED_FIFO):
	SVGA_FIFO_CAPABILITIES = 4,
	SVGA_FIFO_FLAGS = 5,
	SVGA_FIFO_FENCE = 6,
	SVGA_FIFO_3D_HWVERSION = 7,
	SVGA_FIFO_PITCHLOCK = 8,
	SVGA_FIFO_CURSOR_ON = 9,
	SVGA_FIFO_CURSOR_X = 10,
	SVGA_FIFO_CURSOR_Y = 11,
	SVGA_FIFO_CURSOR_COUNT = 12,
	SVGA_FIFO_CURSOR_LAST_UPDATED = 13,
	SVGA_FIFO_RESERVED = 14,
	SVGA_FIFO_CURSOR_SCREEN_ID = 15,
	SVGA_FIFO_DEAD = 16,
	SVGA_FIFO_3D_HWVERSION_REVISED = 17,
	SVGA_FIFO_3D_CAPS = 32,
	SVGA_FIFO_3D_CAPS_LAST = 32 + 255,
	SVGA_FIFO_GUEST_3D_HWVERSION = 288,
	SVGA_FIFO_FENCE_GOAL = 289,
	SVGA_FIFO_BUSY = 290,
	SVGA_FIFO_NUM_REGS = 291,
};

// SVGA_FIFO_CAPABILITIES bits
#define SVGA_FIFO_CAP_NONE 0
#define SVGA_FIFO_CAP_FENCE (1 << 0)
#define SVGA_FIFO_CAP_ACCELFRONT (1 << 1)
#define SVGA_FIFO_CAP_PITCHLOCK (1 << 2)
#define SVGA_FIFO_CAP_VIDEO (1 << 3)
#define SVGA_FIFO_CAP_CURSOR_BYPASS_3 (1 << 4)
#define SVGA_FIFO_CAP_ESCAPE (1 << 5)
#define SVGA_FIFO_CAP_RESERVE (1 << 6)
#define SVGA_FIFO_CAP_SCREEN_OBJECT (1 << 7)
#define SVGA_FIFO_CAP_GMR2 (1 << 8)
#define SVGA_FIFO_CAP_SCREEN_OBJECT_2 (1 << 9)
#define SVGA_FIFO_CAP_DEAD (1 << 10)

// ---------------------------------------------------------------------------
// FIFO commands
// ---------------------------------------------------------------------------
#define SVGA_CMD_INVALID_CMD 0
#define SVGA_CMD_UPDATE 1
#define SVGA_CMD_RECT_COPY 3
#define SVGA_CMD_DEFINE_CURSOR 19
#define SVGA_CMD_DEFINE_ALPHA_CURSOR 22
#define SVGA_CMD_UPDATE_VERBOSE 25
#define SVGA_CMD_FRONT_ROP_FILL 29
#define SVGA_CMD_FENCE 30
#define SVGA_CMD_ESCAPE 33
#define SVGA_CMD_DEFINE_SCREEN 34
#define SVGA_CMD_DESTROY_SCREEN 35
#define SVGA_CMD_DEFINE_GMRFB 36
#define SVGA_CMD_BLIT_GMRFB_TO_SCREEN 37
#define SVGA_CMD_BLIT_SCREEN_TO_GMRFB 38
#define SVGA_CMD_ANNOTATION_FILL 39
#define SVGA_CMD_ANNOTATION_COPY 40
#define SVGA_CMD_DEFINE_GMR2 41
#define SVGA_CMD_REMAP_GMR2 42

// SVGA_CMD_FRONT_ROP_FILL rop value
#define SVGA_ROP_COPY 0x03

// SVGA_CMD_REMAP_GMR2 flags
#define SVGA_REMAP_GMR2_PPN32 0
#define SVGA_REMAP_GMR2_VIA_GMR (1 << 0)
#define SVGA_REMAP_GMR2_PPN64 (1 << 1)
#define SVGA_REMAP_GMR2_SINGLE_PPN (1 << 2)

#define SVGA_GMR_NULL 0xFFFFFFFFU
#define SVGA_GMR_FRAMEBUFFER 0xFFFFFFFEU // the framebuffer BAR as a GMR

// Legacy GMR descriptor (physical page chains via SVGA_REG_GMR_DESCRIPTOR)
typedef struct {
	uint32_t ppn; // physical page number
	uint32_t num_pages; // 0 => ppn is the next descriptor page (chain link)
} svga_guest_mem_descriptor_t;

typedef struct {
	uint32_t gmr_id;
	uint32_t offset;
} svga_guest_ptr_t;

// Screen object flags
#define SVGA_SCREEN_MUST_BE_SET (1 << 0)
#define SVGA_SCREEN_HAS_ROOT SVGA_SCREEN_MUST_BE_SET
#define SVGA_SCREEN_IS_PRIMARY (1 << 1)
#define SVGA_SCREEN_FULLSCREEN_HINT (1 << 2)
#define SVGA_SCREEN_DEACTIVATE (1 << 3)
#define SVGA_SCREEN_BLANKING (1 << 4)

#define SVGA_ID_INVALID_SCREEN 0xFFFFFFFFU

typedef struct {
	uint32_t struct_size; // sizeof(svga_screen_object_t)
	uint32_t id;
	uint32_t flags;
	struct {
		uint32_t width;
		uint32_t height;
	} size;
	struct {
		int32_t x;
		int32_t y;
	} root;
	// Additional fields exist in SCREEN_OBJECT_2; struct_size versions this.
	svga_guest_ptr_t backing_store_ptr;
	uint32_t backing_store_pitch;
	uint32_t clone_count;
} svga_screen_object_t;

// Escape namespaces (SVGA_CMD_ESCAPE)
#define SVGA_ESCAPE_NSID_VMWARE 0x00000000

// Escape command ids in the VMware namespace (video overlay)
#define SVGA_ESCAPE_VMWARE_VIDEO_SET_REGS 0x00020001
#define SVGA_ESCAPE_VMWARE_VIDEO_FLUSH 0x00020002

// Video overlay register ids
enum {
	SVGA_VIDEO_ENABLED = 0,
	SVGA_VIDEO_FLAGS,
	SVGA_VIDEO_DATA_OFFSET,
	SVGA_VIDEO_FORMAT,
	SVGA_VIDEO_COLORKEY,
	SVGA_VIDEO_SIZE,
	SVGA_VIDEO_WIDTH,
	SVGA_VIDEO_HEIGHT,
	SVGA_VIDEO_SRC_X,
	SVGA_VIDEO_SRC_Y,
	SVGA_VIDEO_SRC_WIDTH,
	SVGA_VIDEO_SRC_HEIGHT,
	SVGA_VIDEO_DST_X,
	SVGA_VIDEO_DST_Y,
	SVGA_VIDEO_DST_WIDTH,
	SVGA_VIDEO_DST_HEIGHT,
	SVGA_VIDEO_PITCH_1,
	SVGA_VIDEO_PITCH_2,
	SVGA_VIDEO_PITCH_3,
	SVGA_VIDEO_DATA_GMRID,
	SVGA_VIDEO_DST_SCREEN_ID,
	SVGA_VIDEO_NUM_REGS,
};

// Overlay pixel formats (fourcc)
#define SVGA_VIDEO_FORMAT_YV12 0x32315659
#define SVGA_VIDEO_FORMAT_YUY2 0x32595559
#define SVGA_VIDEO_FORMAT_UYVY 0x59565955

// ---------------------------------------------------------------------------
// 3D (surface) command subset — used only when SVGA_CAP_3D is present
// ---------------------------------------------------------------------------
#define SVGA_3D_CMD_BASE 1040
#define SVGA_3D_CMD_SURFACE_DEFINE (SVGA_3D_CMD_BASE + 0)
#define SVGA_3D_CMD_SURFACE_DESTROY (SVGA_3D_CMD_BASE + 1)
#define SVGA_3D_CMD_SURFACE_COPY (SVGA_3D_CMD_BASE + 2)
#define SVGA_3D_CMD_SURFACE_STRETCHBLT (SVGA_3D_CMD_BASE + 3)
#define SVGA_3D_CMD_SURFACE_DMA (SVGA_3D_CMD_BASE + 4)
#define SVGA_3D_CMD_CONTEXT_DEFINE (SVGA_3D_CMD_BASE + 5)
#define SVGA_3D_CMD_CONTEXT_DESTROY (SVGA_3D_CMD_BASE + 6)
#define SVGA_3D_CMD_PRESENT (SVGA_3D_CMD_BASE + 25)

// Surface formats (subset)
#define SVGA3D_FORMAT_INVALID 0
#define SVGA3D_X8R8G8B8 1
#define SVGA3D_A8R8G8B8 2
#define SVGA3D_R5G6B5 3

// Surface DMA transfer direction
#define SVGA3D_READ_HOST_VRAM 1 // host surface -> guest memory
#define SVGA3D_WRITE_HOST_VRAM 2 // guest memory -> host surface

// Surface flags: none needed for 2D-style usage
#define SVGA3D_SURFACE_HINT_TEXTURE (1 << 2)
#define SVGA3D_SURFACE_HINT_RENDERTARGET (1 << 3)

typedef struct {
	uint32_t id; // SVGA_3D_CMD_*
	uint32_t size; // body size in bytes (excluding this header)
} svga3d_cmd_header_t;

// ---------------------------------------------------------------------------
// Driver limits and objects
// ---------------------------------------------------------------------------
#define SVGA_MAX_SCREENS 8
#define SVGA_MAX_SURFACES 64
#define SVGA_MAX_CONTEXTS 8
#define SVGA_MAX_GMRS 64
#define SVGA_FENCE_TIMEOUT_US 2000000 // 2 s: fence wait give-up
#define SVGA_BUSY_TIMEOUT_US 2000000 // 2 s: legacy sync give-up

// Pixel formats reported to higher layers (color depth reporting)
typedef enum {
	SVGA_PIXFMT_INVALID = 0,
	SVGA_PIXFMT_XRGB8888, // 32 bpp, X8R8G8B8
	SVGA_PIXFMT_RGB565, // 16 bpp, R5G6B5
} svga_pixel_format_t;

// Accel ops exposed to higher layers (EXA-style hooks; all optional, each
// returns <0 when the operation is unavailable on this host)
typedef struct {
	int (*fill)(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
		    uint32_t color); // solid fill on the front buffer
	int (*copy)(uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy,
		    uint32_t w, uint32_t h); // front-buffer screen-to-screen
	int (*sync)(void); // wait for all queued accel ops
} svga_accel_ops_t;

// Mode/topology info for one display (display topology reporting)
typedef struct {
	uint32_t id;
	uint32_t primary;
	int32_t pos_x;
	int32_t pos_y;
	uint32_t width;
	uint32_t height;
} svga_display_info_t;

// ---------------------------------------------------------------------------
// Public driver API
// ---------------------------------------------------------------------------

// Lifecycle
int vmsvga2_init(void); // probe + bring-up; <0 => use GOP fallback
void vmsvga2_shutdown(void);
int vmsvga2_reset(void); // full reinit after FIFO corruption/VM reset
int vmsvga2_suspend(void);
int vmsvga2_resume(void);
int vmsvga2_active(void); // 1 when driver owns the display

// Capabilities / limits
uint32_t vmsvga2_get_caps(void); // SVGA_CAP_* bits
uint32_t vmsvga2_get_fifo_caps(void); // SVGA_FIFO_CAP_* bits
uint32_t vmsvga2_get_vram_size(void);
uint32_t vmsvga2_get_max_width(void);
uint32_t vmsvga2_get_max_height(void);
svga_pixel_format_t vmsvga2_get_pixel_format(void);

// Modesetting
int vmsvga2_set_mode(uint32_t width, uint32_t height, uint32_t bpp);
int vmsvga2_display_enable(int enable);
int vmsvga2_get_info(framebuffer_info_t *out); // current scanout geometry
uint64_t vmsvga2_get_fb_phys(uint64_t *size_out); // FB BAR for mmap
void vmsvga2_set_traces(int enable); // host snoops direct VRAM writes

// Updates and synchronization
void vmsvga2_update_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void vmsvga2_update_full(void);
void vmsvga2_fifo_flush(void); // drain all queued commands
uint32_t vmsvga2_fence_insert(void); // returns fence value (0 if unavail)
int vmsvga2_fence_passed(uint32_t fence);
int vmsvga2_fence_wait(uint32_t fence, uint64_t timeout_us);

// Cursor
int vmsvga2_cursor_define(uint32_t width, uint32_t height, uint32_t hot_x,
			  uint32_t hot_y, const uint32_t *and_mask,
			  const uint32_t *xor_mask);
int vmsvga2_cursor_define_alpha(uint32_t width, uint32_t height,
				uint32_t hot_x, uint32_t hot_y,
				const uint32_t *argb_pixels);
int vmsvga2_cursor_move(int32_t x, int32_t y, int visible);
int vmsvga2_cursor_show(int visible);
int vmsvga2_has_hw_cursor(void);

// GMR (guest memory regions)
int vmsvga2_gmr_alloc(uint32_t num_pages); // returns gmr id or <0
int vmsvga2_gmr_bind(int gmr_id, const uint64_t *page_phys,
		     uint32_t num_pages);
int vmsvga2_gmr_free(int gmr_id);

// Buffer objects (GEM/TTM-like, GMR-backed when possible)
typedef struct svga_bo svga_bo_t;
svga_bo_t *svga_bo_create(uint64_t size);
void svga_bo_ref(svga_bo_t *bo);
void svga_bo_unref(svga_bo_t *bo);
void *svga_bo_map(svga_bo_t *bo); // kernel virtual address
uint64_t svga_bo_size(svga_bo_t *bo);
int svga_bo_gmr_id(svga_bo_t *bo); // bound GMR id, <0 if none

// Screen objects
int vmsvga2_screen_define(uint32_t id, int32_t x, int32_t y, uint32_t w,
			  uint32_t h, uint32_t flags);
int vmsvga2_screen_destroy(uint32_t id);
int vmsvga2_screen_present(uint32_t screen_id, svga_bo_t *bo, uint32_t bo_pitch,
			   int32_t dst_x, int32_t dst_y, uint32_t w,
			   uint32_t h);
int vmsvga2_num_screens(void);

// Surfaces (only when 3D capability present; -1 otherwise)
int vmsvga2_surface_define(uint32_t width, uint32_t height, uint32_t format,
			   uint32_t flags); // returns surface id or <0
int vmsvga2_surface_destroy(int sid);
int vmsvga2_surface_dma_to(int sid, svga_bo_t *bo, uint32_t x, uint32_t y,
			   uint32_t w, uint32_t h, uint32_t pitch);
int vmsvga2_surface_dma_from(int sid, svga_bo_t *bo, uint32_t x, uint32_t y,
			     uint32_t w, uint32_t h, uint32_t pitch);
int vmsvga2_surface_copy(int src_sid, int dst_sid, uint32_t sx, uint32_t sy,
			 uint32_t dx, uint32_t dy, uint32_t w, uint32_t h);
int vmsvga2_surface_present(int sid, uint32_t x, uint32_t y, uint32_t w,
			    uint32_t h);

// 3D contexts (DRI preparation; -1 unless 3D capability present)
int vmsvga2_context_create(void);
int vmsvga2_context_destroy(int cid);

// Accelerated 2D ops (EXA hooks); returns NULL when no accel available
const svga_accel_ops_t *vmsvga2_get_accel_ops(void);

// Video overlay
int vmsvga2_overlay_set(uint32_t unit, const uint32_t *regs,
			uint32_t num_regs);
int vmsvga2_overlay_flush(uint32_t unit);
int vmsvga2_has_overlay(void);

// Topology / EDID
int vmsvga2_get_num_displays(void);
int vmsvga2_get_display_info(uint32_t index, svga_display_info_t *out);
int vmsvga2_get_edid(uint8_t *buf, uint32_t len); // synthesized 128B EDID

// IRQ entry (called from the central interrupt dispatcher).  Returns 1 when
// the device asserted the interrupt (status read+acked), 0 when the line was
// shared and this device was idle.
int vmsvga2_irq(void);
extern volatile int g_vmsvga_initialized;
extern volatile int g_vmsvga_legacy_irq;

// Boot-time best-fit mode selection (same preferred table as the bootloader)
void vmsvga2_setup_boot_mode(void);

#endif // _KERNEL_DEV_VIDEO_VMSVGA2_H_
