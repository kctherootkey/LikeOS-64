// LikeOS-64 Mouse Cursor Loader
// Loads Xcursor format cursor files for mouse pointer display

#ifndef _KERNEL_CURSOR_H_
#define _KERNEL_CURSOR_H_

#include "console.h"

// Xcursor file header (magic "Xcur")
#define XCURSOR_MAGIC       0x72756358  // "Xcur" in little-endian

// Xcursor chunk types
#define XCURSOR_COMMENT     0xfffe0001
#define XCURSOR_IMAGE       0xfffd0002

// Xcursor file header structure
typedef struct {
    uint32_t magic;         // Must be XCURSOR_MAGIC
    uint32_t header;        // Header size in bytes
    uint32_t version;       // File version
    uint32_t ntoc;          // Number of table-of-contents entries
} __attribute__((packed)) xcursor_header_t;

// Xcursor table-of-contents entry
typedef struct {
    uint32_t type;          // Chunk type (COMMENT, IMAGE, etc.)
    uint32_t subtype;       // Subtype (nominal size for images)
    uint32_t position;      // File offset of chunk
} __attribute__((packed)) xcursor_toc_t;

// Xcursor image chunk header
typedef struct {
    uint32_t header;        // Chunk header size
    uint32_t type;          // XCURSOR_IMAGE
    uint32_t subtype;       // Nominal size
    uint32_t version;       // Chunk version
    uint32_t width;         // Image width in pixels
    uint32_t height;        // Image height in pixels
    uint32_t xhot;          // Hotspot X coordinate
    uint32_t yhot;          // Hotspot Y coordinate
    uint32_t delay;         // Animation delay (ms)
    // Followed by width*height ARGB pixels
} __attribute__((packed)) xcursor_image_header_t;

// Maximum cursor dimensions
#define CURSOR_MAX_WIDTH  64
#define CURSOR_MAX_HEIGHT 64

// Loaded cursor data
typedef struct {
    uint32_t* pixels;       // ARGB pixel data
    uint32_t width;         // Width in pixels
    uint32_t height;        // Height in pixels
    uint32_t xhot;          // Hotspot X
    uint32_t yhot;          // Hotspot Y
    uint8_t  loaded;        // 1 if cursor is loaded
} cursor_data_t;

// Cursor loading and management
int cursor_load(const char* path);
int cursor_is_loaded(void);
const cursor_data_t* cursor_get(void);

// Get cursor pixel at position (returns ARGB)
uint32_t cursor_get_pixel(uint32_t x, uint32_t y);

// Get cursor dimensions
uint32_t cursor_get_width(void);
uint32_t cursor_get_height(void);
uint32_t cursor_get_xhot(void);
uint32_t cursor_get_yhot(void);

#endif // _KERNEL_CURSOR_H_
