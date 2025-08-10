// LikeOS-64 Hardware Abstraction Layer - Visual Scrollbar Interface
// Linux Gnome Chrome-style scrollbar rendering system

#ifndef _KERNEL_SCROLLBAR_H_
#define _KERNEL_SCROLLBAR_H_

#include "console.h"

// Scrollbar visual states
#define SCROLLBAR_STATE_NORMAL  0
#define SCROLLBAR_STATE_HOVER   1
#define SCROLLBAR_STATE_PRESSED 2

// Scrollbar dimensions
#define SCROLLBAR_DEFAULT_WIDTH 14      // 14 pixels wide (Chrome-like)
#define SCROLLBAR_MARGIN        3       // 3 pixel margin from edge
#define SCROLLBAR_MIN_HEIGHT    60      // Minimum scrollbar height

// Color scheme (Linux Gnome Chrome Style)
#define SCROLLBAR_TRACK_COLOR      0xF5F5F5    // Very light gray background
#define SCROLLBAR_BUTTON_NORMAL    0xE0E0E0    // Light gray button
#define SCROLLBAR_BUTTON_HOVER     0xD0D0D0    // Slightly darker on hover
#define SCROLLBAR_BUTTON_PRESSED   0xC0C0C0    // Darker when pressed
#define SCROLLBAR_THUMB_NORMAL     0xCCCCCC    // Medium gray thumb
#define SCROLLBAR_THUMB_HOVER      0xBBBBBB    // Darker on hover
#define SCROLLBAR_THUMB_PRESSED    0xAAAAAA    // Darker when pressed
#define SCROLLBAR_BORDER_COLOR     0xCCCCCC    // Border color
#define SCROLLBAR_ARROW_COLOR      0x666666    // Dark gray arrows
#define SCROLLBAR_GRADIENT_LIGHT   0xEEEEEE    // Light gradient color
#define SCROLLBAR_GRADIENT_DARK    0xDDDDDD    // Dark gradient color

// Core scrollbar data structure
typedef struct {
    // Position and dimensions
    uint32_t x, y;              // Top-left position
    uint32_t width, height;     // Total scrollbar dimensions
    
    // Component positions (calculated from dimensions)
    uint32_t button_size;       // Size of up/down buttons (square)
    uint32_t track_y;           // Y position where track starts
    uint32_t track_height;      // Height of track area
    uint32_t thumb_y;           // Y position of thumb
    uint32_t thumb_height;      // Height of thumb
    
    // Visual states (for future mouse interaction)
    uint8_t up_button_state;    // 0=normal, 1=hover, 2=pressed
    uint8_t down_button_state;  // 0=normal, 1=hover, 2=pressed
    uint8_t thumb_state;        // 0=normal, 1=hover, 2=pressed
    
    // Visibility and configuration
    uint8_t visible;            // Whether scrollbar is shown
    uint8_t auto_hide;          // Whether to auto-hide when not needed
    
    // Future scrolling parameters (for interactive version)
    uint32_t total_content;     // Total scrollable content size
    uint32_t visible_content;   // Amount of content visible at once
    uint32_t scroll_position;   // Current scroll position
} scrollbar_t;

// Lightweight content sync descriptor for console integration
typedef struct {
    uint32_t total_lines;     // total_filled_lines (clamped to capacity)
    uint32_t visible_lines;   // rows on screen
    uint32_t viewport_top;    // first visible line (0..max)
} scrollbar_content_t;

// Function prototypes

// Initialization and configuration
int scrollbar_init(scrollbar_t* scrollbar, uint32_t x, uint32_t y, uint32_t height);
void scrollbar_set_visibility(scrollbar_t* scrollbar, uint8_t visible);
void scrollbar_calculate_positions(scrollbar_t* scrollbar);

// Main rendering functions
void scrollbar_render(scrollbar_t* scrollbar);
void scrollbar_render_up_button(scrollbar_t* scrollbar);
void scrollbar_render_down_button(scrollbar_t* scrollbar);
void scrollbar_render_track(scrollbar_t* scrollbar);
void scrollbar_render_thumb(scrollbar_t* scrollbar);

// Content/geometry syncing
void scrollbar_sync_content(scrollbar_t* sb, const scrollbar_content_t* content);
void scrollbar_compute_geometry(scrollbar_t* sb);

// Hit-test helpers (returns 0/1)
int scrollbar_hit_up(const scrollbar_t* sb, uint32_t x, uint32_t y);
int scrollbar_hit_down(const scrollbar_t* sb, uint32_t x, uint32_t y);
int scrollbar_hit_thumb(const scrollbar_t* sb, uint32_t x, uint32_t y);

// Utility drawing functions
void draw_triangle_up(uint32_t x, uint32_t y, uint32_t size, uint32_t color);
void draw_triangle_down(uint32_t x, uint32_t y, uint32_t size, uint32_t color);
void draw_rounded_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, 
                       uint32_t color, uint32_t border_color);
void draw_gradient_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                        uint32_t color_top, uint32_t color_bottom);

// Helper functions
uint32_t get_button_color(uint8_t state);
uint32_t get_thumb_color(uint8_t state);
// Helper functions
uint32_t get_button_color(uint8_t state);
uint32_t get_thumb_color(uint8_t state);
void scrollbar_mark_dirty_region(scrollbar_t* scrollbar);

// Kernel integration functions
int scrollbar_init_system_default(scrollbar_t* scrollbar);
void scrollbar_refresh_system(void);
scrollbar_t* scrollbar_get_system(void);

// State management (for future interactive features)
void scrollbar_set_button_state(scrollbar_t* scrollbar, uint8_t button, uint8_t state);
void scrollbar_set_thumb_state(scrollbar_t* scrollbar, uint8_t state);

// Utility functions for positioning
uint32_t get_screen_width(void);
uint32_t get_screen_height(void);
uint32_t calculate_scrollbar_x_position(uint32_t screen_width);

#endif // _KERNEL_SCROLLBAR_H_
