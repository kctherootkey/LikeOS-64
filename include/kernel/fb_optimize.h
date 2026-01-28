// LikeOS-64 Framebuffer Optimization System
// High-performance double buffering, write-combining, and SSE-optimized rendering

#ifndef _KERNEL_FB_OPTIMIZE_H_
#define _KERNEL_FB_OPTIMIZE_H_

#include "console.h"

// CPU Feature flags
#define CPU_FEATURE_SSE2    (1 << 0)
#define CPU_FEATURE_SSE3    (1 << 1)
#define CPU_FEATURE_SSE4_1  (1 << 2)
#define CPU_FEATURE_SSE4_2  (1 << 3)
#define CPU_FEATURE_MTRR    (1 << 4)

// MTRR types
#define MTRR_TYPE_WB        0x06    // Write-Back
#define MTRR_TYPE_WC        0x01    // Write-Combining
#define MTRR_TYPE_UC        0x00    // Uncacheable

// Dirty rectangle structure for tracking changes
typedef struct {
    uint32_t x1, y1;    // Top-left corner
    uint32_t x2, y2;    // Bottom-right corner
    uint8_t dirty;      // Flag indicating if region needs update
} dirty_rect_t;

// Double buffer system state
typedef struct {
    uint32_t* back_buffer;      // Back buffer in system RAM
    uint32_t* front_buffer;     // Front buffer (actual framebuffer)
    uint32_t width;             // Buffer width in pixels
    uint32_t height;            // Buffer height in pixels
    uint32_t pitch;             // Scanline pitch (pixels per line)
    uint32_t bytes_per_pixel;   // Bytes per pixel (usually 4)
    
    // Dirty region tracking
    dirty_rect_t* dirty_regions;
    uint32_t max_dirty_regions;
    uint32_t num_dirty_regions;
    uint8_t full_screen_dirty;  // Flag for full screen update
    
    // Performance optimization flags
    uint32_t cpu_features;      // Available CPU features
    uint8_t write_combining_enabled;
    uint8_t sse_copy_enabled;
    
    // Statistics
    uint64_t total_updates;
    uint64_t pixels_copied;
    uint64_t dirty_merges;
} fb_double_buffer_t;

// Function prototypes

// System initialization
int fb_optimize_init(framebuffer_info_t* fb_info);
void fb_optimize_shutdown(void);

// Remap front buffer to use direct map (call before removing identity mapping)
void fb_optimize_remap_to_direct_map(void);

// CPU feature detection
uint32_t detect_cpu_features(void);
const char* cpu_features_to_string(uint32_t features);

// Memory type configuration
int configure_write_combining_mtrr(uint64_t fb_base, uint64_t fb_size);
int verify_write_combining(uint64_t fb_base);

// Double buffering operations
void fb_mark_dirty(uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2);
void fb_mark_full_dirty(void);
void fb_flush_dirty_regions(void);
void fb_clear_dirty_regions(void);

// Optimized pixel operations (these replace direct framebuffer access)
void fb_set_pixel(uint32_t x, uint32_t y, uint32_t color);
uint32_t fb_get_pixel(uint32_t x, uint32_t y);
void fb_copy_rect(uint32_t dst_x, uint32_t dst_y, uint32_t src_x, uint32_t src_y, 
                  uint32_t width, uint32_t height);
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color);

// SSE-optimized memory copy routines
void sse_copy_aligned(void* dst, const void* src, size_t bytes);
void sse_copy_unaligned(void* dst, const void* src, size_t bytes);
void* fast_memcpy(void* dst, const void* src, size_t bytes);

// Debug and monitoring
void fb_print_optimization_status(void);
void fb_print_performance_stats(void);
void fb_reset_performance_stats(void);

// Global state access
extern fb_double_buffer_t* get_fb_double_buffer(void);

#endif // _KERNEL_FB_OPTIMIZE_H_
