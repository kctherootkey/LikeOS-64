// LikeOS-64 Mouse Cursor Loader
// Loads Xcursor format cursor files for mouse pointer display

#include "../../include/kernel/cursor.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/vfs.h"
#include "../../include/kernel/memory.h"

// Seek whence values
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

// Global cursor instance
static cursor_data_t g_cursor = {
    .pixels = 0,
    .width = 0,
    .height = 0,
    .xhot = 0,
    .yhot = 0,
    .loaded = 0
};

// Static buffer for cursor pixel data (max 64x64 = 4096 pixels * 4 bytes = 16KB)
static uint32_t g_cursor_pixels[CURSOR_MAX_WIDTH * CURSOR_MAX_HEIGHT];

// Load an Xcursor format cursor file from the VFS
// We look for the smallest image size (typically 24x24) for best fit
int cursor_load(const char* path) {
    vfs_file_t* file = 0;
    int ret;
    long bytes_read;

    // Open the cursor file
    ret = vfs_open(path, 0, &file);
    if (ret != ST_OK || !file) {
        kprintf("cursor: failed to open %s (error %d)\n", path, ret);
        return -1;
    }

    // Read Xcursor header
    xcursor_header_t header;
    bytes_read = vfs_read(file, &header, sizeof(header));
    if (bytes_read != (long)sizeof(header)) {
        kprintf("cursor: failed to read header from %s\n", path);
        vfs_close(file);
        return -1;
    }

    // Verify magic
    if (header.magic != XCURSOR_MAGIC) {
        kprintf("cursor: invalid magic in %s (got 0x%08x)\n", path, header.magic);
        vfs_close(file);
        return -1;
    }

    // Read table of contents
    // We need to find the smallest IMAGE entry (smallest subtype/nominal size)
    uint32_t best_size = 0xFFFFFFFF;
    uint32_t best_position = 0;
    int found_image = 0;

    for (uint32_t i = 0; i < header.ntoc; i++) {
        xcursor_toc_t toc;
        bytes_read = vfs_read(file, &toc, sizeof(toc));
        if (bytes_read != (long)sizeof(toc)) {
            kprintf("cursor: failed to read TOC entry %u\n", i);
            vfs_close(file);
            return -1;
        }

        // Look for IMAGE chunks, prefer smallest nominal size (24 is common)
        if (toc.type == XCURSOR_IMAGE) {
            if (toc.subtype < best_size) {
                best_size = toc.subtype;
                best_position = toc.position;
                found_image = 1;
            }
        }
    }

    if (!found_image) {
        kprintf("cursor: no image found in %s\n", path);
        vfs_close(file);
        return -1;
    }

    // Seek to the best image chunk
    vfs_seek(file, best_position, SEEK_SET);

    // Read image header
    xcursor_image_header_t img_header;
    bytes_read = vfs_read(file, &img_header, sizeof(img_header));
    if (bytes_read != (long)sizeof(img_header)) {
        kprintf("cursor: failed to read image header\n");
        vfs_close(file);
        return -1;
    }

    // Validate dimensions
    if (img_header.width > CURSOR_MAX_WIDTH || img_header.height > CURSOR_MAX_HEIGHT) {
        kprintf("cursor: image too large (%ux%u, max %ux%u)\n",
                img_header.width, img_header.height, CURSOR_MAX_WIDTH, CURSOR_MAX_HEIGHT);
        vfs_close(file);
        return -1;
    }

    if (img_header.width == 0 || img_header.height == 0) {
        kprintf("cursor: invalid image dimensions (%ux%u)\n",
                img_header.width, img_header.height);
        vfs_close(file);
        return -1;
    }

    // Read pixel data (ARGB format, 4 bytes per pixel)
    long pixel_bytes = (long)(img_header.width * img_header.height * 4);
    bytes_read = vfs_read(file, g_cursor_pixels, pixel_bytes);
    if (bytes_read != pixel_bytes) {
        kprintf("cursor: failed to read pixels (read %ld of %ld)\n",
                bytes_read, pixel_bytes);
        vfs_close(file);
        return -1;
    }

    // Store cursor info
    g_cursor.pixels = g_cursor_pixels;
    g_cursor.width = img_header.width;
    g_cursor.height = img_header.height;
    g_cursor.xhot = img_header.xhot;
    g_cursor.yhot = img_header.yhot;
    g_cursor.loaded = 1;

    kprintf("cursor: loaded %s (%ux%u, hotspot %u,%u)\n",
            path, img_header.width, img_header.height, img_header.xhot, img_header.yhot);

    vfs_close(file);
    return 0;
}

// Check if cursor is loaded
int cursor_is_loaded(void) {
    return g_cursor.loaded;
}

// Get cursor structure
const cursor_data_t* cursor_get(void) {
    return g_cursor.loaded ? &g_cursor : 0;
}

// Get cursor pixel at position (returns ARGB)
uint32_t cursor_get_pixel(uint32_t x, uint32_t y) {
    if (!g_cursor.loaded || !g_cursor.pixels) {
        return 0;
    }
    if (x >= g_cursor.width || y >= g_cursor.height) {
        return 0;
    }
    return g_cursor.pixels[y * g_cursor.width + x];
}

// Get cursor width
uint32_t cursor_get_width(void) {
    return g_cursor.loaded ? g_cursor.width : 0;
}

// Get cursor height
uint32_t cursor_get_height(void) {
    return g_cursor.loaded ? g_cursor.height : 0;
}

// Get cursor hotspot X
uint32_t cursor_get_xhot(void) {
    return g_cursor.loaded ? g_cursor.xhot : 0;
}

// Get cursor hotspot Y
uint32_t cursor_get_yhot(void) {
    return g_cursor.loaded ? g_cursor.yhot : 0;
}
