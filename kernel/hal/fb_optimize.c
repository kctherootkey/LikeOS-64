// LikeOS-64 Framebuffer Optimization System - Implementation
// High-performance double buffering, write-combining, and SSE-optimized rendering

#define BOOT_DEBUG 0

#include "../../include/kernel/fb_optimize.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/console.h"


// Define missing types for kernel
typedef unsigned long uintptr_t;

// Global double buffer state
static fb_double_buffer_t g_double_buffer = {0};
static uint8_t g_initialized = 0;

// Maximum number of dirty regions to track
#define MAX_DIRTY_REGIONS 64

// Static buffers for early initialization (before heap is ready)
// Support higher resolutions (e.g. 4K 3840x2160) for notebooks with large panels.
// You can override at compile time by defining MAX_STATIC_FB_WIDTH/HEIGHT.
#ifndef MAX_STATIC_FB_WIDTH
#define MAX_STATIC_FB_WIDTH 1920
#endif
#ifndef MAX_STATIC_FB_HEIGHT
#define MAX_STATIC_FB_HEIGHT 1200
#endif
// 4 bytes per pixel (32-bit RGBA)
#define MAX_STATIC_FB_SIZE (MAX_STATIC_FB_WIDTH * MAX_STATIC_FB_HEIGHT * 4)
static uint32_t g_static_back_buffer[MAX_STATIC_FB_SIZE / sizeof(uint32_t)] __attribute__((aligned(64)));
static dirty_rect_t g_static_dirty_regions[MAX_DIRTY_REGIONS] __attribute__((aligned(64)));
static uint8_t g_using_static_buffers = 0;

// Helper function for string concatenation
static void kstrncat(char* dest, const char* src, size_t dest_size)
{
    size_t dest_len = kstrlen(dest);
    size_t src_len = kstrlen(src);

    if(dest_len + src_len + 1 <= dest_size) {
        kstrcpy(dest + dest_len, src);
    }
}

// CPU feature detection using CPUID
uint32_t detect_cpu_features(void)
{
    uint32_t features = 0;
    uint32_t eax, ebx, ecx, edx;

    // Check for CPUID availability
    __asm__ volatile (
        "pushfq\n\t"
        "pop %%rax\n\t"
        "mov %%rax, %%rbx\n\t"
        "xor $0x200000, %%eax\n\t"
        "push %%rax\n\t"
        "popfq\n\t"
        "pushfq\n\t"
        "pop %%rax\n\t"
        "cmp %%rax, %%rbx\n\t"
        "je cpuid_not_supported\n\t"
        "mov $1, %%eax\n\t"
        "jmp cpuid_supported\n\t"
        "cpuid_not_supported:\n\t"
        "mov $0, %%eax\n\t"
        "cpuid_supported:\n\t"
        : "=a"(eax)
        :
        : "rbx", "memory"
    );

    if(!eax) {
        kprintf("CPUID not supported\n");
        return 0;
    }

    // Get basic CPU features (CPUID leaf 1)
    __asm__ volatile (
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(1)
    );

    // Check SSE2 (bit 26 in EDX)
    if(edx & (1 << 26)) {
        features |= CPU_FEATURE_SSE2;
    }
    // Check SSE3 (bit 0 in ECX)
    if(ecx & (1 << 0)) {
        features |= CPU_FEATURE_SSE3;
    }
    // Check SSE4.1 (bit 19 in ECX)
    if(ecx & (1 << 19)) {
        features |= CPU_FEATURE_SSE4_1;
    }
    // Check SSE4.2 (bit 20 in ECX)
    if(ecx & (1 << 20)) {
        features |= CPU_FEATURE_SSE4_2;
    }
    // Check MTRR support (bit 12 in EDX)
    if(edx & (1 << 12)) {
        features |= CPU_FEATURE_MTRR;
    }
    return features;
}

// Convert CPU features to readable string
const char* cpu_features_to_string(uint32_t features)
{
    static char feature_str[256];
    feature_str[0] = '\0';
    if(features & CPU_FEATURE_SSE2) {
        kstrncat(feature_str, "SSE2 ", sizeof(feature_str));
    }
    if(features & CPU_FEATURE_SSE3) {
        kstrncat(feature_str, "SSE3 ", sizeof(feature_str));
    }
    if(features & CPU_FEATURE_SSE4_1) {
        kstrncat(feature_str, "SSE4.1 ", sizeof(feature_str));
    }
    if(features & CPU_FEATURE_SSE4_2) {
        kstrncat(feature_str, "SSE4.2 ", sizeof(feature_str));
    }
    if(features & CPU_FEATURE_MTRR) {
        kstrncat(feature_str, "MTRR ", sizeof(feature_str));
    }
    return feature_str;
}

// SSE-optimized memory copy for aligned addresses
void sse_copy_aligned(void* dst, const void* src, size_t bytes)
{
    if(!(g_double_buffer.cpu_features & CPU_FEATURE_SSE2) || bytes < 16) {
        // Fallback to regular memory copy
        fast_memcpy(dst, src, bytes);
        return;
    }

    size_t sse_bytes = bytes & ~15; // Round down to 16-byte boundary
    size_t remaining = bytes - sse_bytes;

    __asm__ volatile (
        "1:\n\t"
        "movdqa (%0), %%xmm0\n\t"
        "movdqa %%xmm0, (%1)\n\t"
        "add $16, %0\n\t"
        "add $16, %1\n\t"
        "sub $16, %2\n\t"
        "jnz 1b"
        : "+r"(src), "+r"(dst), "+r"(sse_bytes)
        :
        : "memory", "xmm0"
    );

    // Copy remaining bytes
    if(remaining) {
        char* dst_byte = (char*)dst;
        const char* src_byte = (const char*)src;
        for(size_t i = 0; i < remaining; i++) {
            dst_byte[i] = src_byte[i];
        }
    }
}

// SSE-optimized memory copy for unaligned addresses
void sse_copy_unaligned(void* dst, const void* src, size_t bytes)
{
    if(!(g_double_buffer.cpu_features & CPU_FEATURE_SSE2) || bytes < 16) {
        // Fallback to regular memory copy
        fast_memcpy(dst, src, bytes);
        return;
    }

    size_t sse_bytes = bytes & ~15; // Round down to 16-byte boundary
    size_t remaining = bytes - sse_bytes;

    __asm__ volatile (
        "1:\n\t"
        "movdqu (%0), %%xmm0\n\t"
        "movdqu %%xmm0, (%1)\n\t"
        "add $16, %0\n\t"
        "add $16, %1\n\t"
        "sub $16, %2\n\t"
        "jnz 1b"
        : "+r"(src), "+r"(dst), "+r"(sse_bytes)
        :
        : "memory", "xmm0"
    );

    // Copy remaining bytes
    if(remaining) {
        char* dst_byte = (char*)dst;
        const char* src_byte = (const char*)src;
        for(size_t i = 0; i < remaining; i++) {
            dst_byte[i] = src_byte[i];
        }
    }
}

// Fast memory copy with automatic alignment detection
void* fast_memcpy(void* dst, const void* src, size_t bytes)
{
    if(!bytes) {
        return dst;
    }
    // If bytes are small (<16) or SSE2 unavailable, do scalar copy to avoid recursion
    if(!(g_double_buffer.cpu_features & CPU_FEATURE_SSE2) || bytes < 16) {
        char* d = (char*)dst;
        const char* s = (const char*)src;
        // Try to use 64-bit copies for large-enough, aligned chunks
        if(bytes >= 8 && (((uintptr_t)d & 7) == 0) && (((uintptr_t)s & 7) == 0)) {
            uint64_t* d64 = (uint64_t*)d;
            const uint64_t* s64 = (const uint64_t*)s;
            size_t count64 = bytes / 8;
            for(size_t i = 0; i < count64; i++) {
                d64[i] = s64[i];
            }
            bytes %= 8;
            d += count64 * 8;
            s += count64 * 8;
        }
        for(size_t i = 0; i < bytes; i++) {
            d[i] = s[i];
        }
        return dst;
    }
    // SSE path for >=16 bytes
    uintptr_t src_align = (uintptr_t)src & 15;
    uintptr_t dst_align = (uintptr_t)dst & 15;
    if(src_align == 0 && dst_align == 0) {
        sse_copy_aligned(dst, src, bytes);
    } else {
        sse_copy_unaligned(dst, src, bytes);
    }
    return dst;
}

// Initialize the framebuffer optimization system
int fb_optimize_init(framebuffer_info_t* fb_info)
{
    if(!fb_info || !fb_info->framebuffer_base) {
#if BOOT_DEBUG
        kprintf("FB Optimize: Invalid framebuffer info\n");
#endif
        return -1;
    }
    if(g_initialized) {
#if BOOT_DEBUG
        kprintf("FB Optimize: Already initialized\n");
#endif
        return 0;
    }
#if BOOT_DEBUG
    kprintf("Initializing framebuffer optimization system...\n");
#endif
    // Detect CPU features
    g_double_buffer.cpu_features = detect_cpu_features();
#if BOOT_DEBUG
    kprintf("  CPU Features: %s\n", cpu_features_to_string(g_double_buffer.cpu_features));
#endif
    // Store framebuffer info
    g_double_buffer.front_buffer = (uint32_t*)fb_info->framebuffer_base;
    g_double_buffer.width = fb_info->horizontal_resolution;
    g_double_buffer.height = fb_info->vertical_resolution;
    g_double_buffer.pitch = fb_info->pixels_per_scanline;
    g_double_buffer.bytes_per_pixel = fb_info->bytes_per_pixel;
    // Calculate back buffer size
    size_t buffer_size = g_double_buffer.height * g_double_buffer.pitch * sizeof(uint32_t);
#if BOOT_DEBUG
    kprintf("  Framebuffer: %dx%d, pitch=%d, size=%zu bytes\n",
        g_double_buffer.width, g_double_buffer.height,
        g_double_buffer.pitch, buffer_size);
#endif
    // Try to use static buffer first if size fits and no kalloc available yet
    if(buffer_size <= MAX_STATIC_FB_SIZE) {
        g_double_buffer.back_buffer = g_static_back_buffer;
        g_using_static_buffers = 1;
#if BOOT_DEBUG
        kprintf("  Using static back buffer %p (size fits: %zu <= %zu)\n", g_static_back_buffer, buffer_size, (size_t)MAX_STATIC_FB_SIZE);
#endif
    } else {
        // Try dynamic allocation for larger framebuffers
        g_double_buffer.back_buffer = (uint32_t*)kalloc(buffer_size);
        if(!g_double_buffer.back_buffer) {
#if BOOT_DEBUG
            kprintf("  ERROR: Framebuffer too large for static buffer (%zu > %zu) and kalloc failed\n",
                buffer_size, (size_t)MAX_STATIC_FB_SIZE);
#endif
            return -1;
        }
        g_using_static_buffers = 0;
#if BOOT_DEBUG
        kprintf("  Back buffer dynamically allocated at: %p\n", g_double_buffer.back_buffer);
#endif
    }
    // Always use static dirty regions array (small and fixed size)
    g_double_buffer.max_dirty_regions = MAX_DIRTY_REGIONS;
    g_double_buffer.dirty_regions = g_static_dirty_regions;
    // Initialize dirty regions
    g_double_buffer.num_dirty_regions = 0;
    g_double_buffer.full_screen_dirty = 1; // Start with full screen dirty
    // Configure write-combining if available
    uint64_t fb_base = (uint64_t)fb_info->framebuffer_base;
    uint64_t fb_size = fb_info->framebuffer_size;
    if(g_double_buffer.cpu_features & CPU_FEATURE_MTRR) {
        if(configure_write_combining_mtrr(fb_base, fb_size) == 0) {
            g_double_buffer.write_combining_enabled = 1;
#if BOOT_DEBUG
            kprintf("  Write-combining enabled via MTRR\n");
#endif
        }
    }
    // Enable SSE copying if available
    if(g_double_buffer.cpu_features & CPU_FEATURE_SSE2) {
        g_double_buffer.sse_copy_enabled = 1;
#if BOOT_DEBUG
        kprintf("  SSE-optimized copying enabled\n");
#endif
    }
    // Initialize statistics
    g_double_buffer.total_updates = 0;
    g_double_buffer.pixels_copied = 0;
    g_double_buffer.dirty_merges = 0;
    // Copy current framebuffer content to back buffer
    fast_memcpy(g_double_buffer.back_buffer, g_double_buffer.front_buffer, buffer_size);
    g_initialized = 1;
#if BOOT_DEBUG
    kprintf("Framebuffer optimization system initialized successfully\n");
#endif
    return 0;
}

// Shutdown the optimization system
void fb_optimize_shutdown(void)
{
    if(!g_initialized) {
        return;
    }
    // Final flush of dirty regions
    fb_flush_dirty_regions();
    // Free allocated memory (only if using dynamic allocation)
    if(g_double_buffer.back_buffer && !g_using_static_buffers) {
        kfree(g_double_buffer.back_buffer);
    }
    g_double_buffer.back_buffer = NULL;
    // Dirty regions always use static allocation, so don't free
    g_double_buffer.dirty_regions = NULL;
    g_initialized = 0;
    g_using_static_buffers = 0;
    kprintf("Framebuffer optimization system shutdown\n");
}

// Remap front buffer to use direct map (call before removing identity mapping)
void fb_optimize_remap_to_direct_map(void)
{
    if(!g_initialized || !g_double_buffer.front_buffer) {
        return;
    }
    // The front buffer was stored as an identity-mapped physical address
    // Convert it to use the direct map at PHYS_MAP_BASE
    uint64_t fb_phys = (uint64_t)g_double_buffer.front_buffer;
    g_double_buffer.front_buffer = (uint32_t*)phys_to_virt(fb_phys);
}

// Merge overlapping dirty regions to optimize copy operations
static int merge_dirty_regions(void)
{
    if(g_double_buffer.num_dirty_regions < 2) {
        return 0;
    }
    int merges = 0;
    for(uint32_t i = 0; i < g_double_buffer.num_dirty_regions; i++) {
        if(!g_double_buffer.dirty_regions[i].dirty) {
            continue;
        }
        for(uint32_t j = i + 1; j < g_double_buffer.num_dirty_regions; j++) {
            if(!g_double_buffer.dirty_regions[j].dirty) {
                continue;
            }
            dirty_rect_t* r1 = &g_double_buffer.dirty_regions[i];
            dirty_rect_t* r2 = &g_double_buffer.dirty_regions[j];
            // Check if regions overlap or are adjacent
            if(r1->x1 <= r2->x2 + 1 && r1->x2 + 1 >= r2->x1 &&
               r1->y1 <= r2->y2 + 1 && r1->y2 + 1 >= r2->y1) {
                // Merge regions
                r1->x1 = (r1->x1 < r2->x1) ? r1->x1 : r2->x1;
                r1->y1 = (r1->y1 < r2->y1) ? r1->y1 : r2->y1;
                r1->x2 = (r1->x2 > r2->x2) ? r1->x2 : r2->x2;
                r1->y2 = (r1->y2 > r2->y2) ? r1->y2 : r2->y2;
                // Mark second region as inactive
                r2->dirty = 0;
                merges++;
                g_double_buffer.dirty_merges++;
            }
        }
    }
    // Compact the array by removing inactive regions
    if(merges > 0) {
        uint32_t write_idx = 0;
        for(uint32_t read_idx = 0; read_idx < g_double_buffer.num_dirty_regions; read_idx++) {
            if(g_double_buffer.dirty_regions[read_idx].dirty) {
                if(write_idx != read_idx) {
                    g_double_buffer.dirty_regions[write_idx] = g_double_buffer.dirty_regions[read_idx];
                }
                write_idx++;
            }
        }
        g_double_buffer.num_dirty_regions = write_idx;
    }
    return merges;
}

// Copy a dirty region from back buffer to front buffer
static void copy_region_to_front(uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2)
{
    uint32_t width = x2 - x1 + 1;
    uint32_t height = y2 - y1 + 1;
    for(uint32_t y = 0; y < height; y++) {
        uint32_t src_offset = (y1 + y) * g_double_buffer.pitch + x1;
        uint32_t dst_offset = src_offset;
        uint32_t* src = &g_double_buffer.back_buffer[src_offset];
        uint32_t* dst = &g_double_buffer.front_buffer[dst_offset];
        fast_memcpy(dst, src, width * sizeof(uint32_t));
    }
    g_double_buffer.pixels_copied += width * height;
}

// Mark a rectangular region as dirty
void fb_mark_dirty(uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2)
{
    if(!g_initialized) {
        return;
    }
    // Clamp coordinates to screen bounds
    if(x1 >= g_double_buffer.width) {
        x1 = g_double_buffer.width - 1;
    }
    if(y1 >= g_double_buffer.height) {
        y1 = g_double_buffer.height - 1;
    }
    if(x2 >= g_double_buffer.width) {
        x2 = g_double_buffer.width - 1;
    }
    if(y2 >= g_double_buffer.height) {
        y2 = g_double_buffer.height - 1;
    }
    // Ensure x1,y1 is top-left and x2,y2 is bottom-right
    if(x1 > x2) {
        uint32_t tmp = x1;
        x1 = x2;
        x2 = tmp;
    }
    if(y1 > y2) {
        uint32_t tmp = y1;
        y1 = y2;
        y2 = tmp;
    }
    // If we're at maximum dirty regions, mark full screen dirty
    if(g_double_buffer.num_dirty_regions >= g_double_buffer.max_dirty_regions) {
        g_double_buffer.full_screen_dirty = 1;
        return;
    }
    // Add new dirty region
    dirty_rect_t* region = &g_double_buffer.dirty_regions[g_double_buffer.num_dirty_regions];
    region->x1 = x1;
    region->y1 = y1;
    region->x2 = x2;
    region->y2 = y2;
    region->dirty = 1;
    g_double_buffer.num_dirty_regions++;
    // Try to merge overlapping regions to optimize performance
    merge_dirty_regions();
}

// Mark the entire screen as dirty
void fb_mark_full_dirty(void)
{
    if(!g_initialized) {
        return;
    }
    g_double_buffer.full_screen_dirty = 1;
    g_double_buffer.num_dirty_regions = 0;
}

// Flush all dirty regions to the front buffer
void fb_flush_dirty_regions(void)
{
    if(!g_initialized) {
        return;
    }
    g_double_buffer.total_updates++;
    if(g_double_buffer.full_screen_dirty) {
        // Copy entire screen
        size_t buffer_size = g_double_buffer.height * g_double_buffer.pitch * sizeof(uint32_t);
        fast_memcpy(g_double_buffer.front_buffer, g_double_buffer.back_buffer, buffer_size);
        g_double_buffer.pixels_copied += g_double_buffer.width * g_double_buffer.height;
        g_double_buffer.full_screen_dirty = 0;
        g_double_buffer.num_dirty_regions = 0;
        return;
    }
    // Copy individual dirty regions
    for(uint32_t i = 0; i < g_double_buffer.num_dirty_regions; i++) {
        dirty_rect_t* region = &g_double_buffer.dirty_regions[i];
        if(region->dirty) {
            // Clamp region to bounds defensively
            uint32_t x1 = region->x1;
            uint32_t y1 = region->y1;
            uint32_t x2 = region->x2;
            uint32_t y2 = region->y2;
            if(x1 >= g_double_buffer.width) {
                x1 = g_double_buffer.width ? g_double_buffer.width - 1 : 0;
            }
            if(y1 >= g_double_buffer.height) {
                y1 = g_double_buffer.height ? g_double_buffer.height - 1 : 0;
            }
            if(x2 >= g_double_buffer.width) {
                x2 = g_double_buffer.width ? g_double_buffer.width - 1 : 0;
            }
            if(y2 >= g_double_buffer.height) {
                y2 = g_double_buffer.height ? g_double_buffer.height - 1 : 0;
            }
            if(x1 > x2 || y1 > y2) {
                continue;
            }
            uint32_t w = x2 - x1 + 1;
            uint32_t h = y2 - y1 + 1;
            if(!w || !h) {
                continue;
            }
            // Sanity check offsets
            uint64_t max_index = (uint64_t)(g_double_buffer.height - 1) * g_double_buffer.pitch + (g_double_buffer.width - 1);
            uint64_t start_index = (uint64_t)y1 * g_double_buffer.pitch + x1;
            uint64_t end_index = (uint64_t)y2 * g_double_buffer.pitch + x2;
            if(start_index > max_index || end_index > max_index) {
                continue;
            }
            copy_region_to_front(x1, y1, x2, y2);
        }
    }
    // Clear dirty regions
    g_double_buffer.num_dirty_regions = 0;
}

// Clear all dirty regions without flushing
void fb_clear_dirty_regions(void)
{
    if(!g_initialized) {
        return;
    }
    g_double_buffer.num_dirty_regions = 0;
    g_double_buffer.full_screen_dirty = 0;
}

// Optimized pixel set operation (writes to back buffer)
void fb_set_pixel(uint32_t x, uint32_t y, uint32_t color)
{
    if(!g_initialized) {
        return;
    }
    if(x >= g_double_buffer.width || y >= g_double_buffer.height) {
        return;
    }
    uint32_t offset = y * g_double_buffer.pitch + x;
    g_double_buffer.back_buffer[offset] = color;
    // Mark single pixel as dirty
    fb_mark_dirty(x, y, x, y);
}

// Get pixel from back buffer (never read from front buffer)
uint32_t fb_get_pixel(uint32_t x, uint32_t y)
{
    if(!g_initialized) {
        return 0;
    }
    if(x >= g_double_buffer.width || y >= g_double_buffer.height) {
        return 0;
    }
    uint32_t offset = y * g_double_buffer.pitch + x;
    return g_double_buffer.back_buffer[offset];
}

// Copy rectangle within back buffer
void fb_copy_rect(uint32_t dst_x, uint32_t dst_y, uint32_t src_x, uint32_t src_y,
          uint32_t width, uint32_t height)
{
    if(!g_initialized) {
        return;
    }
    // Bounds checking
    if(dst_x + width > g_double_buffer.width) {
        width = g_double_buffer.width - dst_x;
    }
    if(dst_y + height > g_double_buffer.height) {
        height = g_double_buffer.height - dst_y;
    }
    if(src_x + width > g_double_buffer.width) {
        width = g_double_buffer.width - src_x;
    }
    if(src_y + height > g_double_buffer.height) {
        height = g_double_buffer.height - src_y;
    }
    if(width == 0 || height == 0) {
        return;
    }
    // Copy line by line to handle overlapping regions correctly
    if(dst_y > src_y || (dst_y == src_y && dst_x > src_x)) {
        // Copy from bottom to top, right to left
        for(int32_t y = height - 1; y >= 0; y--) {
            uint32_t src_offset = (src_y + y) * g_double_buffer.pitch + src_x;
            uint32_t dst_offset = (dst_y + y) * g_double_buffer.pitch + dst_x;
            // Use memmove-like behavior for overlapping copies
            for(int32_t x = width - 1; x >= 0; x--) {
                g_double_buffer.back_buffer[dst_offset + x] = g_double_buffer.back_buffer[src_offset + x];
            }
        }
    } else {
        // Copy from top to bottom, left to right
        for(uint32_t y = 0; y < height; y++) {
            uint32_t src_offset = (src_y + y) * g_double_buffer.pitch + src_x;
            uint32_t dst_offset = (dst_y + y) * g_double_buffer.pitch + dst_x;
            fast_memcpy(&g_double_buffer.back_buffer[dst_offset],
                   &g_double_buffer.back_buffer[src_offset],
                   width * sizeof(uint32_t));
        }
    }
    // Mark destination area as dirty
    fb_mark_dirty(dst_x, dst_y, dst_x + width - 1, dst_y + height - 1);
}

// Fill rectangle in back buffer
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color)
{
    if(!g_initialized) {
        return;
    }
    // Bounds checking
    if(x >= g_double_buffer.width || y >= g_double_buffer.height) {
        return;
    }
    if(x + width > g_double_buffer.width) {
        width = g_double_buffer.width - x;
    }
    if(y + height > g_double_buffer.height) {
        height = g_double_buffer.height - y;
    }
    if(width == 0 || height == 0) {
        return;
    }
    // Fill rectangle
    for(uint32_t row = 0; row < height; row++) {
        uint32_t offset = (y + row) * g_double_buffer.pitch + x;
        for(uint32_t col = 0; col < width; col++) {
            g_double_buffer.back_buffer[offset + col] = color;
        }
    }
    // Mark area as dirty
    fb_mark_dirty(x, y, x + width - 1, y + height - 1);
}

// Get access to the global double buffer state (for integration)
fb_double_buffer_t* get_fb_double_buffer(void)
{
    return g_initialized ? &g_double_buffer : NULL;
}

// Print optimization status
void fb_print_optimization_status(void)
{
    if(!g_initialized) {
        kprintf("Framebuffer optimization: Not initialized\n");
        return;
    }
    kprintf("=== Framebuffer Optimization Status ===\n");
    kprintf("Resolution: %dx%d (pitch: %d)\n",
        g_double_buffer.width, g_double_buffer.height, g_double_buffer.pitch);
    kprintf("Back buffer: %p\n", g_double_buffer.back_buffer);
    kprintf("Front buffer: %p\n", g_double_buffer.front_buffer);
    kprintf("CPU Features: %s\n", cpu_features_to_string(g_double_buffer.cpu_features));
    kprintf("Write-combining: %s\n", g_double_buffer.write_combining_enabled ? "Enabled" : "Disabled");
    kprintf("SSE copying: %s\n", g_double_buffer.sse_copy_enabled ? "Enabled" : "Disabled");
    kprintf("Dirty regions: %d/%d\n", g_double_buffer.num_dirty_regions, g_double_buffer.max_dirty_regions);
    kprintf("Full screen dirty: %s\n", g_double_buffer.full_screen_dirty ? "Yes" : "No");
}

// Print performance statistics
void fb_print_performance_stats(void)
{
    if(!g_initialized) {
        kprintf("Framebuffer optimization: Not initialized\n");
        return;
    }
    kprintf("=== Framebuffer Performance Statistics ===\n");
    kprintf("Total updates: %llu\n", g_double_buffer.total_updates);
    kprintf("Pixels copied: %llu\n", g_double_buffer.pixels_copied);
    kprintf("Dirty region merges: %llu\n", g_double_buffer.dirty_merges);
    if(g_double_buffer.total_updates > 0) {
        uint64_t avg_pixels = g_double_buffer.pixels_copied / g_double_buffer.total_updates;
        uint64_t total_pixels = g_double_buffer.width * g_double_buffer.height;
        uint32_t efficiency = (uint32_t)((avg_pixels * 100) / total_pixels);
        kprintf("Average pixels per update: %llu (%d%% of screen)\n", avg_pixels, efficiency);
    }
}

// Reset performance statistics
void fb_reset_performance_stats(void)
{
    if(!g_initialized) {
        return;
    }
    g_double_buffer.total_updates = 0;
    g_double_buffer.pixels_copied = 0;
    g_double_buffer.dirty_merges = 0;
}

// MTRR register definitions
#define MSR_MTRRcap         0x0FE
#define MSR_MTRRdefType     0x2FF
#define MSR_MTRRphysBase0   0x200
#define MSR_MTRRphysMask0   0x201

// Read MSR
static uint64_t read_msr(uint32_t msr)
{
    uint32_t low, high;
    __asm__ volatile (
        "rdmsr"
        : "=a"(low), "=d"(high)
        : "c"(msr)
    );
    return ((uint64_t)high << 32) | low;
}

// Write MSR
static void write_msr(uint32_t msr, uint64_t value)
{
    uint32_t low = (uint32_t)(value & 0xFFFFFFFF);
    uint32_t high = (uint32_t)(value >> 32);
    __asm__ volatile (
        "wrmsr"
        :
        : "c"(msr), "a"(low), "d"(high)
    );
}

// Configure write-combining using MTRRs
int configure_write_combining_mtrr(uint64_t fb_base, uint64_t fb_size)
{
    if(!(g_double_buffer.cpu_features & CPU_FEATURE_MTRR)) {
        return -1;
    }
#if BOOT_DEBUG
    kprintf("  Configuring MTRR for write-combining...\n");
#endif
    // Read MTRR capabilities
    uint64_t mtrr_cap = read_msr(MSR_MTRRcap);
    uint32_t num_var_mtrrs = (uint32_t)(mtrr_cap & 0xFF);
#if BOOT_DEBUG
    kprintf("    MTRR cap: 0x%llx, variable MTRRs: %d\n", mtrr_cap, num_var_mtrrs);
#endif
    if(num_var_mtrrs == 0) {
#if BOOT_DEBUG
        kprintf("    No variable MTRRs available\n");
#endif
        return -1;
    }
    // Find the largest power-of-2 size that fits in fb_size
    uint64_t mtrr_size = 1;
    while(mtrr_size < fb_size) {
        mtrr_size <<= 1;
    }
    // If the computed size is larger than fb_size, use the previous power of 2
    if(mtrr_size > fb_size) {
        mtrr_size >>= 1;
    }
    // Align base address to the MTRR size
    uint64_t mtrr_base = fb_base & ~(mtrr_size - 1);
#if BOOT_DEBUG
    kprintf("    Setting MTRR: base=0x%llx, size=0x%llx, type=WC\n", mtrr_base, mtrr_size);
#endif
    // Find an available MTRR
    int mtrr_index = -1;
    for(uint32_t i = 0; i < num_var_mtrrs; i++) {
        uint64_t mask = read_msr(MSR_MTRRphysMask0 + (i * 2));
        if(!(mask & (1ULL << 11))) { // Check if MTRR is available (valid bit clear)
            mtrr_index = i;
            break;
        }
    }
    if(mtrr_index == -1) {
#if BOOT_DEBUG
        kprintf("    No available MTRR slots\n");
#endif
        return -1;
    }
    // Disable MTRRs while modifying
    uint64_t mtrr_def_type = read_msr(MSR_MTRRdefType);
    write_msr(MSR_MTRRdefType, mtrr_def_type & ~(1ULL << 11));
    // Set up the MTRR
    uint64_t phys_base = mtrr_base | MTRR_TYPE_WC;
    uint64_t phys_mask = (~(mtrr_size - 1) & ((1ULL << 36) - 1)) | (1ULL << 11);
    write_msr(MSR_MTRRphysBase0 + (mtrr_index * 2), phys_base);
    write_msr(MSR_MTRRphysMask0 + (mtrr_index * 2), phys_mask);
    // Re-enable MTRRs
    write_msr(MSR_MTRRdefType, mtrr_def_type | (1ULL << 11));
    // Flush caches and TLB
    __asm__ volatile (
        "wbinvd\n\t"
        "mov %%cr3, %%rax\n\t"
        "mov %%rax, %%cr3"
        :
        :
        : "rax", "memory"
    );
#if BOOT_DEBUG
    kprintf("    MTRR %d configured successfully\n", mtrr_index);
#endif
    return 0;
}

// Verify write-combining is working
int verify_write_combining(uint64_t fb_base)
{
    // This is a basic verification - read the MTRR settings
    if(!(g_double_buffer.cpu_features & CPU_FEATURE_MTRR)) {
        return -1;
    }
    uint64_t mtrr_cap = read_msr(MSR_MTRRcap);
    uint32_t num_var_mtrrs = (uint32_t)(mtrr_cap & 0xFF);
    for(uint32_t i = 0; i < num_var_mtrrs; i++) {
        uint64_t base = read_msr(MSR_MTRRphysBase0 + (i * 2));
        uint64_t mask = read_msr(MSR_MTRRphysMask0 + (i * 2));
        if(mask & (1ULL << 11)) { // Valid bit set
            uint64_t mtrr_base = base & ~0xFFF;
            uint8_t mtrr_type = base & 0xFF;
            // Check if this MTRR covers our framebuffer
            if(mtrr_type == MTRR_TYPE_WC && mtrr_base <= fb_base) {
                uint64_t mtrr_size = 1;
                uint64_t temp_mask = mask >> 12;
                while(!(temp_mask & 1)) {
                    mtrr_size <<= 1;
                    temp_mask >>= 1;
                }
                if(fb_base < mtrr_base + mtrr_size) {
                    kprintf("    Write-combining verified: MTRR %d covers 0x%llx\n", i, fb_base);
                    return 0;
                }
            }
        }
    }
    return -1;
}
