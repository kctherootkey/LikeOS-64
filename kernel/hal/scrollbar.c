// LikeOS-64 Hardware Abstraction Layer - Visual Scrollbar Implementation
// Linux Gnome Chrome-style scrollbar rendering system

#include "../../include/kernel/scrollbar.h"
#include "../../include/kernel/fb_optimize.h"
#include "../../include/kernel/memory.h"

#define NULL ((void*)0)

// Initialize scrollbar with position and dimensions
int scrollbar_init(scrollbar_t* scrollbar, uint32_t x, uint32_t y, uint32_t height) {
    if (!scrollbar) return -1;
    
    // Set basic properties
    scrollbar->x = x;
    scrollbar->y = y;
    scrollbar->width = SCROLLBAR_DEFAULT_WIDTH;
    scrollbar->height = height;
    
    // Initialize visual states
    scrollbar->up_button_state = SCROLLBAR_STATE_NORMAL;
    scrollbar->down_button_state = SCROLLBAR_STATE_NORMAL;
    scrollbar->thumb_state = SCROLLBAR_STATE_NORMAL;
    
    // Set visibility and configuration
    scrollbar->visible = 1;
    scrollbar->auto_hide = 0;
    
    // Initialize future scrolling parameters
    scrollbar->total_content = 0;
    scrollbar->visible_content = 0;
    scrollbar->scroll_position = 0;
    
    // Calculate component positions
    scrollbar_calculate_positions(scrollbar);
    
    return 0;
}

// Calculate positions of scrollbar components
void scrollbar_calculate_positions(scrollbar_t* scrollbar) {
    if (!scrollbar) return;
    
    // Button size is same as width (square buttons)
    scrollbar->button_size = scrollbar->width;
    
    // Track starts after up button
    scrollbar->track_y = scrollbar->y + scrollbar->button_size;
    scrollbar->track_height = scrollbar->height - (2 * scrollbar->button_size);
    
    // Default thumb until content sync sets real values
    scrollbar->thumb_height = (scrollbar->track_height > 0) ? (scrollbar->track_height / 4) : 0;
    if (scrollbar->thumb_height < 8) scrollbar->thumb_height = 8; // Minimum size
    scrollbar->thumb_y = scrollbar->track_y;
}

// Set scrollbar visibility
void scrollbar_set_visibility(scrollbar_t* scrollbar, uint8_t visible) {
    if (!scrollbar) return;
    scrollbar->visible = visible;
}

// Main rendering function - renders entire scrollbar
void scrollbar_render(scrollbar_t* scrollbar) {
    if (!scrollbar || !scrollbar->visible) return;
    
    // Render all components
    scrollbar_render_track(scrollbar);
    scrollbar_render_up_button(scrollbar);
    scrollbar_render_down_button(scrollbar);
    if (scrollbar->thumb_height > 0) {
        scrollbar_render_thumb(scrollbar);
    }
    
    // Mark the entire scrollbar area as dirty
    scrollbar_mark_dirty_region(scrollbar);
}

// Render the up arrow button
void scrollbar_render_up_button(scrollbar_t* scrollbar) {
    if (!scrollbar || !scrollbar->visible) return;
    
    uint32_t x = scrollbar->x;
    uint32_t y = scrollbar->y;
    uint32_t size = scrollbar->button_size;
    
    // Get button color based on state
    uint32_t bg_color = get_button_color(scrollbar->up_button_state);
    
    // Draw button background with gradient
    draw_gradient_rect(x, y, size, size, bg_color + 0x101010, bg_color);
    
    // Draw button border
    // Top border
    fb_fill_rect(x, y, size, 1, SCROLLBAR_BORDER_COLOR);
    // Left border
    fb_fill_rect(x, y, 1, size, SCROLLBAR_BORDER_COLOR);
    // Right border
    fb_fill_rect(x + size - 1, y, 1, size, SCROLLBAR_BORDER_COLOR);
    // Bottom border
    fb_fill_rect(x, y + size - 1, size, 1, SCROLLBAR_BORDER_COLOR);
    
    // Draw up arrow (triangle pointing up)
    uint32_t arrow_x = x + size / 2;
    uint32_t arrow_y = y + size / 2;
    uint32_t arrow_size = size / 3;
    if (arrow_size < 3) arrow_size = 3;
    
    draw_triangle_up(arrow_x, arrow_y, arrow_size, SCROLLBAR_ARROW_COLOR);
}

// Render the down arrow button
void scrollbar_render_down_button(scrollbar_t* scrollbar) {
    if (!scrollbar || !scrollbar->visible) return;
    
    uint32_t x = scrollbar->x;
    uint32_t y = scrollbar->y + scrollbar->height - scrollbar->button_size;
    uint32_t size = scrollbar->button_size;
    
    // Get button color based on state
    uint32_t bg_color = get_button_color(scrollbar->down_button_state);
    
    // Draw button background with gradient
    draw_gradient_rect(x, y, size, size, bg_color + 0x101010, bg_color);
    
    // Draw button border
    // Top border
    fb_fill_rect(x, y, size, 1, SCROLLBAR_BORDER_COLOR);
    // Left border
    fb_fill_rect(x, y, 1, size, SCROLLBAR_BORDER_COLOR);
    // Right border
    fb_fill_rect(x + size - 1, y, 1, size, SCROLLBAR_BORDER_COLOR);
    // Bottom border
    fb_fill_rect(x, y + size - 1, size, 1, SCROLLBAR_BORDER_COLOR);
    
    // Draw down arrow (triangle pointing down)
    uint32_t arrow_x = x + size / 2;
    uint32_t arrow_y = y + size / 2;
    uint32_t arrow_size = size / 3;
    if (arrow_size < 3) arrow_size = 3;
    
    draw_triangle_down(arrow_x, arrow_y, arrow_size, SCROLLBAR_ARROW_COLOR);
}

// Render the scroll track (background area)
void scrollbar_render_track(scrollbar_t* scrollbar) {
    if (!scrollbar || !scrollbar->visible) return;
    
    uint32_t x = scrollbar->x;
    uint32_t y = scrollbar->track_y;
    uint32_t width = scrollbar->width;
    uint32_t height = scrollbar->track_height;
    
    // Fill track with very light gray background
    fb_fill_rect(x, y, width, height, SCROLLBAR_TRACK_COLOR);
    
    // Add subtle inset border for depth
    // Left inner shadow (1 pixel darker)
    fb_fill_rect(x, y, 1, height, SCROLLBAR_TRACK_COLOR - 0x0A0A0A);
    // Top inner shadow (1 pixel darker)
    fb_fill_rect(x, y, width, 1, SCROLLBAR_TRACK_COLOR - 0x0A0A0A);
}

// Render the scroll thumb (draggable indicator)
void scrollbar_render_thumb(scrollbar_t* scrollbar) {
    if (!scrollbar || !scrollbar->visible) return;
    
    uint32_t x = scrollbar->x + 1; // 1 pixel inset from track edge
    uint32_t y = scrollbar->thumb_y;
    uint32_t width = (scrollbar->width > 2) ? (scrollbar->width - 2) : scrollbar->width; // 2px inset guard
    uint32_t height = scrollbar->thumb_height;
    if (height == 0 || width == 0) return;
    
    // Get thumb color based on state
    uint32_t bg_color = get_thumb_color(scrollbar->thumb_state);
    
    // Draw thumb with rounded corners effect
    draw_rounded_rect(x, y, width, height, bg_color, SCROLLBAR_BORDER_COLOR - 0x222222);
    
    // Add subtle gradient effect
    if (width > 2 && height > 2) {
        draw_gradient_rect(x + 1, y + 1, width - 2, height - 2,
                           bg_color + 0x101010, bg_color - 0x101010);
    }
}

// Draw an upward-pointing triangle
void draw_triangle_up(uint32_t center_x, uint32_t center_y, uint32_t size, uint32_t color) {
    // Draw a simple upward triangle using horizontal lines
    for (uint32_t i = 0; i < size; i++) {
        uint32_t y = center_y - size/2 + i;
        uint32_t x_start = center_x - i;
        uint32_t width = 2 * i + 1;
        
        fb_fill_rect(x_start, y, width, 1, color);
    }
}

// Draw a downward-pointing triangle
void draw_triangle_down(uint32_t center_x, uint32_t center_y, uint32_t size, uint32_t color) {
    // Draw a simple downward triangle using horizontal lines
    for (uint32_t i = 0; i < size; i++) {
        uint32_t y = center_y - size/2 + i;
        uint32_t x_start = center_x - (size - 1 - i);
        uint32_t width = 2 * (size - 1 - i) + 1;
        
        fb_fill_rect(x_start, y, width, 1, color);
    }
}

// Draw a rectangle with rounded corners (simplified version)
void draw_rounded_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, 
                       uint32_t color, uint32_t border_color) {
    // For now, draw regular rectangle with border
    // In future, can add proper corner rounding
    
    // Fill main area
    fb_fill_rect(x, y, width, height, color);
    
    // Draw border
    // Top
    fb_fill_rect(x, y, width, 1, border_color);
    // Bottom
    fb_fill_rect(x, y + height - 1, width, 1, border_color);
    // Left
    fb_fill_rect(x, y, 1, height, border_color);
    // Right
    fb_fill_rect(x + width - 1, y, 1, height, border_color);
    
    // Simple corner rounding - remove/adjust corner pixels if size permits
    if (width > 4 && height > 4) {
        uint32_t x1 = x;
        uint32_t y1 = y;
        uint32_t x2 = x + width - 1;
        uint32_t y2 = y + height - 1;
        // Top-left corner
        fb_set_pixel(x1, y1, border_color);
        fb_set_pixel(x1 + 1, y1, color);
        fb_set_pixel(x1, y1 + 1, color);
        // Top-right corner
        fb_set_pixel(x2, y1, border_color);
        if (x2 > 0) fb_set_pixel(x2 - 1, y1, color);
        fb_set_pixel(x2, y1 + 1, color);
        // Bottom-left corner
        fb_set_pixel(x1, y2, border_color);
        if (y2 > 0) fb_set_pixel(x1, y2 - 1, color);
        fb_set_pixel(x1 + 1, y2, color);
        // Bottom-right corner
        fb_set_pixel(x2, y2, border_color);
        if (x2 > 0) fb_set_pixel(x2 - 1, y2, color);
        if (y2 > 0) fb_set_pixel(x2, y2 - 1, color);
    }
}

// Draw a rectangle with vertical gradient
void draw_gradient_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                        uint32_t color_top, uint32_t color_bottom) {
    if (height == 0) return;
    
    // Extract RGB components
    uint8_t r_top = (color_top >> 16) & 0xFF;
    uint8_t g_top = (color_top >> 8) & 0xFF;
    uint8_t b_top = color_top & 0xFF;
    
    uint8_t r_bottom = (color_bottom >> 16) & 0xFF;
    uint8_t g_bottom = (color_bottom >> 8) & 0xFF;
    uint8_t b_bottom = color_bottom & 0xFF;
    
    // Draw gradient line by line
    for (uint32_t row = 0; row < height; row++) {
        // Calculate interpolation factor (0-255)
        uint32_t factor = (row * 255) / (height - 1);
        
        // Interpolate each color component
        uint8_t r = ((r_top * (255 - factor)) + (r_bottom * factor)) / 255;
        uint8_t g = ((g_top * (255 - factor)) + (g_bottom * factor)) / 255;
        uint8_t b = ((b_top * (255 - factor)) + (b_bottom * factor)) / 255;
        
        uint32_t line_color = (r << 16) | (g << 8) | b;
        fb_fill_rect(x, y + row, width, 1, line_color);
    }
}

// Get button color based on state
uint32_t get_button_color(uint8_t state) {
    switch (state) {
        case SCROLLBAR_STATE_HOVER:   return SCROLLBAR_BUTTON_HOVER;
        case SCROLLBAR_STATE_PRESSED: return SCROLLBAR_BUTTON_PRESSED;
        default:                      return SCROLLBAR_BUTTON_NORMAL;
    }
}

// Get thumb color based on state
uint32_t get_thumb_color(uint8_t state) {
    switch (state) {
        case SCROLLBAR_STATE_HOVER:   return SCROLLBAR_THUMB_HOVER;
        case SCROLLBAR_STATE_PRESSED: return SCROLLBAR_THUMB_PRESSED;
        default:                      return SCROLLBAR_THUMB_NORMAL;
    }
}

// Mark scrollbar area as dirty for rendering
void scrollbar_mark_dirty_region(scrollbar_t* scrollbar) {
    if (!scrollbar || !scrollbar->visible) return;
    
    fb_mark_dirty(scrollbar->x, scrollbar->y, 
                  scrollbar->x + scrollbar->width - 1, 
                  scrollbar->y + scrollbar->height - 1);
}

// Set button state (0=up button, 1=down button)
void scrollbar_set_button_state(scrollbar_t* scrollbar, uint8_t button, uint8_t state) {
    if (!scrollbar) return;
    
    if (button == 0) {
        scrollbar->up_button_state = state;
    } else {
        scrollbar->down_button_state = state;
    }
}

// Set thumb state
void scrollbar_set_thumb_state(scrollbar_t* scrollbar, uint8_t state) {
    if (!scrollbar) return;
    scrollbar->thumb_state = state;
}

// Global system scrollbar
static scrollbar_t* g_system_scrollbar = NULL;

// Get screen dimensions from framebuffer
uint32_t get_screen_width(void) {
    fb_double_buffer_t* fb_buffer = get_fb_double_buffer();
    return fb_buffer ? fb_buffer->width : 800; // Default fallback
}

uint32_t get_screen_height(void) {
    fb_double_buffer_t* fb_buffer = get_fb_double_buffer();
    return fb_buffer ? fb_buffer->height : 600; // Default fallback
}

// Calculate scrollbar X position (right edge with margin)
uint32_t calculate_scrollbar_x_position(uint32_t screen_width) {
    return screen_width - SCROLLBAR_DEFAULT_WIDTH - SCROLLBAR_MARGIN;
}

// Initialize and render main system scrollbar
int scrollbar_init_system_default(scrollbar_t* scrollbar) {
    if (!scrollbar) return -1;
    
    // Get screen dimensions
    uint32_t screen_width = get_screen_width();
    uint32_t screen_height = get_screen_height();
    
    // Calculate position (right edge with margin)
    uint32_t scrollbar_x = calculate_scrollbar_x_position(screen_width);
    uint32_t scrollbar_y = 0;  // Start from top
    uint32_t scrollbar_height = screen_height;  // Full height
    
    // Initialize scrollbar
    if (scrollbar_init(scrollbar, scrollbar_x, scrollbar_y, scrollbar_height) != 0) {
        return -1;
    }
    
    // Store reference to system scrollbar
    g_system_scrollbar = scrollbar;
    
    return 0;
}

// Refresh the system scrollbar (re-render it)
void scrollbar_refresh_system(void) {
    if (g_system_scrollbar) {
        scrollbar_render(g_system_scrollbar);
        fb_flush_dirty_regions();
    }
}

// Get pointer to system scrollbar
scrollbar_t* scrollbar_get_system(void) {
    return g_system_scrollbar;
}

// ===== New helpers for content/geometry sync and hit-testing =====
static uint32_t clampu32(uint32_t v, uint32_t lo, uint32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void scrollbar_compute_geometry(scrollbar_t* sb) {
    if (!sb) return;
    sb->button_size = sb->width;
    sb->track_y = sb->y + sb->button_size;
    sb->track_height = (sb->height > 2 * sb->button_size) ? (sb->height - 2 * sb->button_size) : 0;
    if (sb->track_height == 0) {
        sb->thumb_height = 0;
        sb->thumb_y = sb->track_y;
        return;
    }
    // Compute thumb height proportional to visible/total
    if (sb->total_content == 0 || sb->visible_content == 0 || sb->total_content <= sb->visible_content) {
        sb->thumb_height = sb->track_height; // all content fits
        sb->thumb_y = sb->track_y;
        return;
    }
    uint32_t min_thumb = 8;
    uint32_t h = (uint32_t)((uint64_t)sb->track_height * (uint64_t)sb->visible_content / (uint64_t)sb->total_content);
    if (h < min_thumb) h = min_thumb;
    if (h > sb->track_height) h = sb->track_height;
    sb->thumb_height = h;
    // Map scroll_position to thumb_y within track
    uint32_t max_scroll = sb->total_content - sb->visible_content;
    uint32_t track_range = (sb->track_height > sb->thumb_height) ? (sb->track_height - sb->thumb_height) : 0;
    uint32_t ty = sb->track_y;
    if (max_scroll > 0 && track_range > 0) {
        ty = sb->track_y + (uint32_t)((uint64_t)sb->scroll_position * (uint64_t)track_range / (uint64_t)max_scroll);
    }
    sb->thumb_y = ty;
}

void scrollbar_sync_content(scrollbar_t* sb, const scrollbar_content_t* content) {
    if (!sb || !content) return;
    sb->total_content = content->total_lines;
    sb->visible_content = content->visible_lines;
    sb->scroll_position = content->viewport_top;
    scrollbar_compute_geometry(sb);
}

int scrollbar_hit_up(const scrollbar_t* sb, uint32_t x, uint32_t y) {
    if (!sb || !sb->visible) return 0;
    return (x >= sb->x && x < sb->x + sb->width && y >= sb->y && y < sb->y + sb->button_size);
}

int scrollbar_hit_down(const scrollbar_t* sb, uint32_t x, uint32_t y) {
    if (!sb || !sb->visible) return 0;
    uint32_t by = sb->y + sb->height - sb->button_size;
    return (x >= sb->x && x < sb->x + sb->width && y >= by && y < by + sb->button_size);
}

int scrollbar_hit_thumb(const scrollbar_t* sb, uint32_t x, uint32_t y) {
    if (!sb || !sb->visible) return 0;
    // Thumb is inset by 1px on X in render; accept within track width
    uint32_t tx = sb->x + 1;
    uint32_t tw = (sb->width > 2) ? (sb->width - 2) : sb->width;
    return (x >= tx && x < tx + tw && y >= sb->thumb_y && y < sb->thumb_y + sb->thumb_height);
}
