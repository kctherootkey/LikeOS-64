// LikeOS-64 System Font Loader
// Loads PSF1 (PC Screen Font v1) fonts for console display.
//
// The console addresses glyphs by Unicode code point, not by byte, so what
// matters in the file is not just the bitmaps but the Unicode table that
// follows them: a list, one entry per glyph, of the code points that glyph
// renders.  PSF1 carries that table as UCS-2, which covers the Basic
// Multilingual Plane -- everything a fixed-width console font holds glyphs for.

#ifndef _KERNEL_SYSFONT_H_
#define _KERNEL_SYSFONT_H_

#include <kernel/io/console.h>

// PSF1 header structure
typedef struct {
	uint8_t magic[2]; // 0x36, 0x04
	uint8_t mode; // Font mode flags (below)
	uint8_t charsize; // Bytes per glyph = height in pixels; width is always 8
} __attribute__((packed)) psf1_header_t;

// PSF1 magic values
#define PSF1_MAGIC0 0x36
#define PSF1_MAGIC1 0x04

// PSF1 mode flags
#define PSF1_MODE512 0x01 // 512 glyphs rather than 256
#define PSF1_MODEHASTAB 0x02 // a Unicode table follows the glyph data
#define PSF1_MODEHASSEQ 0x04 // that table contains combining sequences

// PSF1 Unicode table markers, as 16-bit values in the table itself.
#define PSF1_STARTSEQ 0xFFFE // introduces a combining sequence
#define PSF1_SEPARATOR 0xFFFF // ends one glyph's entry

// Returned by sysfont_glyph_index() when the font has no glyph for a code point
#define SYSFONT_NO_GLYPH 0xFFFFFFFFu

// One Unicode table entry: a code point and the glyph that renders it.
// The table is kept sorted by cp so that lookup is a binary search.
typedef struct {
	uint32_t cp;
	uint32_t glyph;
} sysfont_unimap_t;

// Font data structure for console use
typedef struct {
	const uint8_t *glyphs; // Glyph bitmaps, numglyphs * bytesperglyph
	uint32_t numglyphs; // Number of glyphs in font
	uint32_t width; // Character width in pixels
	uint32_t height; // Character height in pixels
	uint32_t bytesperglyph; // Bytes per glyph
	const sysfont_unimap_t *unimap; // Sorted code point -> glyph table
	uint32_t nunimap; // Entries in unimap (0 if font has no table)
	uint32_t replacement; // Glyph index for unmapped code points
	uint8_t loaded; // 1 if a font is loaded
} sysfont_t;

// Font loading and management
int sysfont_load(const char *path);
int sysfont_is_loaded(void);
const sysfont_t *sysfont_get(void);

// Map a Unicode code point to a glyph index, or SYSFONT_NO_GLYPH when the font
// does not cover it.  Callers that want to render regardless should use
// sysfont_get_glyph(), which substitutes a replacement glyph.
uint32_t sysfont_glyph_index(uint32_t codepoint);

// Get the glyph bitmap for a Unicode code point.  Never returns NULL for a
// loaded font: an unmapped code point yields U+FFFD, '?' or glyph 0.
const uint8_t *sysfont_get_glyph(uint32_t codepoint);

// Get font dimensions
uint32_t sysfont_get_width(void);
uint32_t sysfont_get_height(void);

#endif // _KERNEL_SYSFONT_H_
