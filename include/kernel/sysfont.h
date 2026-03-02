// LikeOS-64 System Font Loader
// Loads PSF (PC Screen Font) fonts for console display

#ifndef _KERNEL_SYSFONT_H_
#define _KERNEL_SYSFONT_H_

#include "console.h"

// PSF1 header structure (legacy format)
typedef struct {
    uint8_t magic[2];     // 0x36, 0x04
    uint8_t mode;         // Font mode flags
    uint8_t charsize;     // Character height in bytes (width is always 8)
} __attribute__((packed)) psf1_header_t;

// PSF2 header structure (modern format)
typedef struct {
    uint8_t  magic[4];       // 0x72, 0xb5, 0x4a, 0x86
    uint32_t version;        // Zero
    uint32_t headersize;     // Offset of bitmaps in file, usually 32
    uint32_t flags;          // 0 if no unicode table
    uint32_t numglyph;       // Number of glyphs
    uint32_t bytesperglyph;  // Bytes per glyph
    uint32_t height;         // Height in pixels
    uint32_t width;          // Width in pixels
} __attribute__((packed)) psf2_header_t;

// PSF magic values
#define PSF1_MAGIC0     0x36
#define PSF1_MAGIC1     0x04
#define PSF2_MAGIC0     0x72
#define PSF2_MAGIC1     0xb5
#define PSF2_MAGIC2     0x4a
#define PSF2_MAGIC3     0x86

// Font data structure for console use
typedef struct {
    uint8_t* glyphs;      // Pointer to glyph bitmap data
    uint32_t numglyphs;   // Number of glyphs in font
    uint32_t width;       // Character width in pixels
    uint32_t height;      // Character height in pixels
    uint32_t bytesperglyph; // Bytes per glyph
    uint8_t  loaded;      // 1 if font is loaded
} sysfont_t;

// Font loading and management
int sysfont_load(const char* path);
int sysfont_is_loaded(void);
const sysfont_t* sysfont_get(void);

// Get glyph bitmap for a character
const uint8_t* sysfont_get_glyph(unsigned char c);

// Get font dimensions
uint32_t sysfont_get_width(void);
uint32_t sysfont_get_height(void);

#endif // _KERNEL_SYSFONT_H_
