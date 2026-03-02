// LikeOS-64 System Font Loader
// Loads PSF (PC Screen Font) fonts for console display

#include "../../include/kernel/sysfont.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/vfs.h"
#include "../../include/kernel/memory.h"

// Seek whence values
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

// Global system font instance
static sysfont_t g_sysfont = {
    .glyphs = 0,
    .numglyphs = 0,
    .width = 0,
    .height = 0,
    .bytesperglyph = 0,
    .loaded = 0
};

// Buffer for font glyph data (statically allocated to avoid heap fragmentation)
// Max size: 512 glyphs * 64 bytes per glyph = 32KB
#define SYSFONT_MAX_GLYPHS 512
#define SYSFONT_MAX_BYTES_PER_GLYPH 64
static uint8_t g_glyph_buffer[SYSFONT_MAX_GLYPHS * SYSFONT_MAX_BYTES_PER_GLYPH];

// Load a PSF font file from the VFS
int sysfont_load(const char* path) {
    vfs_file_t* file = 0;
    int ret;
    long bytes_read;

    // Open the font file
    ret = vfs_open(path, 0, &file);
    if (ret != ST_OK || !file) {
        kprintf("sysfont: failed to open %s (error %d)\n", path, ret);
        return -1;
    }

    // Read magic bytes to determine format
    uint8_t magic[4];
    bytes_read = vfs_read(file, magic, 4);
    if (bytes_read < 4) {
        kprintf("sysfont: failed to read magic from %s\n", path);
        vfs_close(file);
        return -1;
    }

    // Seek back to start
    vfs_seek(file, 0, SEEK_SET);

    // Check for PSF2 format (most common on modern Linux)
    if (magic[0] == PSF2_MAGIC0 && magic[1] == PSF2_MAGIC1 &&
        magic[2] == PSF2_MAGIC2 && magic[3] == PSF2_MAGIC3) {
        
        // Read PSF2 header
        psf2_header_t header;
        bytes_read = vfs_read(file, &header, sizeof(header));
        if (bytes_read != (long)sizeof(header)) {
            kprintf("sysfont: failed to read PSF2 header\n");
            vfs_close(file);
            return -1;
        }

        // Validate header
        if (header.width > 16 || header.height > 32) {
            kprintf("sysfont: unsupported PSF2 font size %ux%u\n", header.width, header.height);
            vfs_close(file);
            return -1;
        }

        if (header.numglyph > SYSFONT_MAX_GLYPHS) {
            kprintf("sysfont: too many glyphs (%u, max %u)\n", header.numglyph, SYSFONT_MAX_GLYPHS);
            vfs_close(file);
            return -1;
        }

        if (header.bytesperglyph > SYSFONT_MAX_BYTES_PER_GLYPH) {
            kprintf("sysfont: glyph too large (%u bytes, max %u)\n", header.bytesperglyph, SYSFONT_MAX_BYTES_PER_GLYPH);
            vfs_close(file);
            return -1;
        }

        // Seek to glyph data (skip header to headersize offset)
        vfs_seek(file, header.headersize, SEEK_SET);

        // Read all glyph data
        long glyph_data_size = (long)(header.numglyph * header.bytesperglyph);
        bytes_read = vfs_read(file, g_glyph_buffer, glyph_data_size);
        if (bytes_read != glyph_data_size) {
            kprintf("sysfont: failed to read PSF2 glyphs (read %ld of %ld)\n", 
                    bytes_read, glyph_data_size);
            vfs_close(file);
            return -1;
        }

        // Store font info
        g_sysfont.glyphs = g_glyph_buffer;
        g_sysfont.numglyphs = header.numglyph;
        g_sysfont.width = header.width;
        g_sysfont.height = header.height;
        g_sysfont.bytesperglyph = header.bytesperglyph;
        g_sysfont.loaded = 1;

        kprintf("sysfont: loaded PSF2 font %s (%ux%u, %u glyphs)\n", 
                path, header.width, header.height, header.numglyph);

        vfs_close(file);
        return 0;
    }
    // Check for PSF1 format (legacy)
    else if (magic[0] == PSF1_MAGIC0 && magic[1] == PSF1_MAGIC1) {
        
        // Read PSF1 header
        psf1_header_t header;
        bytes_read = vfs_read(file, &header, sizeof(header));
        if (bytes_read != (long)sizeof(header)) {
            kprintf("sysfont: failed to read PSF1 header\n");
            vfs_close(file);
            return -1;
        }

        // PSF1 has fixed width of 8 pixels, height = charsize
        uint32_t width = 8;
        uint32_t height = header.charsize;
        uint32_t numglyphs = (header.mode & 0x01) ? 512 : 256;
        uint32_t bytesperglyph = header.charsize;

        if (numglyphs > SYSFONT_MAX_GLYPHS) {
            numglyphs = SYSFONT_MAX_GLYPHS;
        }

        if (bytesperglyph > SYSFONT_MAX_BYTES_PER_GLYPH) {
            kprintf("sysfont: PSF1 glyph too large (%u bytes)\n", bytesperglyph);
            vfs_close(file);
            return -1;
        }

        // Read all glyph data
        long glyph_data_size = (long)(numglyphs * bytesperglyph);
        bytes_read = vfs_read(file, g_glyph_buffer, glyph_data_size);
        if (bytes_read != glyph_data_size) {
            kprintf("sysfont: failed to read PSF1 glyphs (read %ld of %ld)\n",
                    bytes_read, glyph_data_size);
            vfs_close(file);
            return -1;
        }

        // Store font info
        g_sysfont.glyphs = g_glyph_buffer;
        g_sysfont.numglyphs = numglyphs;
        g_sysfont.width = width;
        g_sysfont.height = height;
        g_sysfont.bytesperglyph = bytesperglyph;
        g_sysfont.loaded = 1;

        kprintf("sysfont: loaded PSF1 font %s (%ux%u, %u glyphs)\n",
                path, width, height, numglyphs);

        vfs_close(file);
        return 0;
    }
    else {
        kprintf("sysfont: unknown font format in %s (magic: %02x %02x %02x %02x)\n",
                path, magic[0], magic[1], magic[2], magic[3]);
        vfs_close(file);
        return -1;
    }
}

// Check if system font is loaded
int sysfont_is_loaded(void) {
    return g_sysfont.loaded;
}

// Get system font structure
const sysfont_t* sysfont_get(void) {
    return g_sysfont.loaded ? &g_sysfont : NULL;
}

// Get glyph bitmap for a character
const uint8_t* sysfont_get_glyph(unsigned char c) {
    if (!g_sysfont.loaded || !g_sysfont.glyphs) {
        return NULL;
    }
    
    // Clamp to available glyphs
    if (c >= g_sysfont.numglyphs) {
        c = '?';  // Fallback to question mark for unknown chars
        if (c >= g_sysfont.numglyphs) {
            c = 0;  // Last resort
        }
    }
    
    return &g_sysfont.glyphs[(uint32_t)c * g_sysfont.bytesperglyph];
}

// Get font width
uint32_t sysfont_get_width(void) {
    return g_sysfont.loaded ? g_sysfont.width : 8;
}

// Get font height
uint32_t sysfont_get_height(void) {
    return g_sysfont.loaded ? g_sysfont.height : 16;
}
