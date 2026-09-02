// LikeOS-64 Framebuffer Optimization System
// High-performance double buffering, write-combining, and SSE-optimized rendering

#ifndef _KERNEL_FB_OPTIMIZE_H_
#define _KERNEL_FB_OPTIMIZE_H_

#include <kernel/io/console.h>

// CPU Feature flags
#define CPU_FEATURE_SSE2 (1 << 0)
#define CPU_FEATURE_SSE3 (1 << 1)
#define CPU_FEATURE_SSE4_1 (1 << 2)
#define CPU_FEATURE_SSE4_2 (1 << 3)
#define CPU_FEATURE_MTRR (1 << 4)

// MTRR types
#define MTRR_TYPE_WB 0x06 // Write-Back
#define MTRR_TYPE_WC 0x01 // Write-Combining
#define MTRR_TYPE_UC 0x00 // Uncacheable

// Dirty rectangle structure for tracking changes
typedef struct {
	uint32_t x1, y1; // Top-left corner
	uint32_t x2, y2; // Bottom-right corner
	uint8_t dirty; // Flag indicating if region needs update
} dirty_rect_t;

// Double buffer system state
typedef struct {
	uint32_t *back_buffer; // Back buffer in system RAM
	uint32_t *front_buffer; // Front buffer (actual framebuffer)
	uint32_t width; // Buffer width in pixels
	uint32_t height; // Buffer height in pixels
	uint32_t pitch; // Scanline pitch (pixels per line)
	uint32_t bytes_per_pixel; // Bytes per pixel (usually 4)

	// Dirty region tracking
	dirty_rect_t *dirty_regions;
	uint32_t max_dirty_regions;
	uint32_t num_dirty_regions;
	uint8_t full_screen_dirty; // Flag for full screen update

	// Performance optimization flags
	uint32_t cpu_features; // Available CPU features
	uint8_t write_combining_enabled;
	uint8_t sse_copy_enabled;

	// Statistics
	uint64_t total_updates;
	uint64_t pixels_copied;
	uint64_t dirty_merges;
} fb_double_buffer_t;

// Function prototypes

// System initialization
int fb_optimize_init(framebuffer_info_t *fb_info);
void fb_optimize_shutdown(void);

// Re-initialize for a new mode (runtime resolution change).  Swaps the back
// buffer and geometry; does NOT touch MTRR/PAT write-combining (the display
// driver configures caching for its framebuffer once at probe time).
int fb_reinit(framebuffer_info_t *fb_info);

// Post-flush notification hook: called AFTER dirty regions were copied to the
// front buffer and AFTER fb_lock is released, with the bounding box of the
// flushed area.  The SVGA driver uses this to send screen-update commands.
typedef void (*fb_flush_hook_t)(uint32_t x, uint32_t y, uint32_t w,
				uint32_t h);
void fb_set_flush_hook(fb_flush_hook_t hook);
// The hook currently installed, so a driver that replaces it can put back
// what was there if its own takeover fails.
fb_flush_hook_t fb_get_flush_hook(void);
// Push one rectangle through the hook immediately.  For a writer that put
// pixels into the front buffer itself and so has no dirty region for the
// console's flush path to find -- /dev/fb0 writes.  Goes through the hook
// rather than to any particular display driver, so the same call works
// whether the screen is driven by the display manager or by the framebuffer
// driver underneath it.
void fb_flush_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
// Run the hook for any pending flushed area.  fb_flush_dirty_regions() calls
// this automatically; callers of the _unlocked variant must invoke it after
// releasing the framebuffer lock.
void fb_flush_hook_run(void);

// Remap front buffer to use direct map (call before removing identity mapping)
void fb_optimize_remap_to_direct_map(void);

// CPU feature detection
uint32_t detect_cpu_features(void);
const char *cpu_features_to_string(uint32_t features);

// Memory type configuration
int configure_write_combining_mtrr(uint64_t fb_base, uint64_t fb_size);
int verify_write_combining(uint64_t fb_base);

// SMP-safe framebuffer locking
// Use these to perform atomic multi-pixel operations (e.g., cursor drawing)
void fb_acquire(uint64_t *saved_flags);
void fb_release(uint64_t saved_flags);

// Unlocked variants - use ONLY when caller already holds fb_lock via fb_acquire()
void fb_set_pixel_unlocked(uint32_t x, uint32_t y, uint32_t color);
uint32_t fb_get_pixel_unlocked(uint32_t x, uint32_t y);
void fb_mark_dirty_unlocked(uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2);
void fb_flush_dirty_regions_unlocked(void);

// Double buffering operations
void fb_mark_dirty(uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2);
void fb_mark_full_dirty(void);
void fb_flush_dirty_regions(void);
void fb_clear_dirty_regions(void);

// Optimized pixel operations (these replace direct framebuffer access)
void fb_set_pixel(uint32_t x, uint32_t y, uint32_t color);
uint32_t fb_get_pixel(uint32_t x, uint32_t y);
void fb_copy_rect(uint32_t dst_x, uint32_t dst_y, uint32_t src_x,
		  uint32_t src_y, uint32_t width, uint32_t height);
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
		  uint32_t color);

// SSE-optimized memory copy routines
void sse_copy_aligned(void *dst, const void *src, size_t bytes);
void sse_copy_unaligned(void *dst, const void *src, size_t bytes);
void sse_copy_nt(void *dst, const void *src, size_t bytes);
void *fast_memcpy(void *dst, const void *src, size_t bytes);

// PAT-based write-combining configuration
// Reprograms PAT MSR entry 1 to WC, then sets PWT on framebuffer 2MB pages
int configure_pat_write_combining(uint64_t fb_phys_base, uint64_t fb_size);

// Program THIS CPU's IA32_PAT entry 1 to WC.  IA32_PAT is per-logical-CPU and
// must be identical on all processors: the BSP gets it via
// configure_pat_write_combining(); every AP MUST call this from ap_entry(), or
// the framebuffer (whose PDEs select PAT entry 1) is WC on the BSP but
// effective-UC on that AP -- flushes then run ~90x slower depending on which
// CPU performs them.
void fb_pat_program_wc_this_cpu(void);

// Fast character drawing: writes directly to back buffer, marks dirty once
void fb_draw_char_fast(uint32_t x, uint32_t y, const uint8_t *glyph,
		       uint32_t font_w, uint32_t font_h, uint32_t bytes_per_row,
		       uint32_t fg_color, uint32_t bg_color);

// Debug and monitoring
void fb_print_optimization_status(void);
void fb_print_performance_stats(void);
void fb_reset_performance_stats(void);

// Global state access
/* ---- Pointer overlay ---------------------------------------------------
 *
 * The mouse pointer is composited into the FRONT buffer as the last step of
 * every flush, and is never written into the back buffer.
 *
 * It used to be drawn into the back buffer, after saving the pixels it
 * covered so they could be put back when it moved.  That save is a snapshot,
 * and anything the console drew under the pointer made it stale: putting it
 * back then stamped the old pixels over the new text.  A scroll was worse --
 * fb_copy_rect moved the drawn pointer along with the text, leaving a second
 * copy behind, and a second scroll left a third.  Hiding the pointer around
 * every operation that draws would mean finding all of them and never missing
 * one; compositing at flush time means there is no saved background to go
 * stale and no drawn copy to be carried around, so there is nothing to leave
 * behind.
 *
 * `argb` is width*height pixels with straight alpha in the high byte, and
 * belongs to the caller: it must stay valid until replaced.
 */
void fb_pointer_set_image(const uint32_t *argb, uint32_t width,
			  uint32_t height);
void fb_pointer_set_visible(int visible);
/* Move the pointer.  Marks both the vacated and the newly covered rectangle
 * dirty, so the next flush repaints the first from the back buffer and
 * composites the pointer into the second. */
void fb_pointer_move(int x, int y);
/* Mark the pointer's current rectangle dirty without moving it -- used when
 * something else has overwritten the screen (a display server exiting). */
void fb_pointer_damage(void);

extern fb_double_buffer_t *get_fb_double_buffer(void);

#endif // _KERNEL_FB_OPTIMIZE_H_
