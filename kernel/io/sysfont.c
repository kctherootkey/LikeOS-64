// LikeOS-64 System Font Loader
// Loads PSF1 (PC Screen Font v1) fonts for console display.
//
// The console addresses glyphs by Unicode code point, so the interesting part
// of a font file is not just the bitmaps but the Unicode table that follows
// them: a list, one entry per glyph, of the code points that glyph renders.
// That table is turned here into a sorted code point -> glyph array which
// sysfont_glyph_index() binary-searches, plus a direct-indexed fast path for
// Latin-1 (the range essentially all console output falls in).

#include <kernel/io/sysfont.h>
#include <kernel/io/console.h>
#include <kernel/fs/vfs.h>
#include <kernel/mm/memory.h>
#include <kernel/uapi/stat.h>
#include <kernel/uapi/bug.h>

// Seek whence values
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

// PSF1 fixes the width at 8, and one mode bit decides between 256 and 512
// glyphs, so neither needs a runtime bound.  The height does: it comes off disk
// as a single byte, and the console renderer will not draw a taller cell.
#define SYSFONT_WIDTH 8u
#define SYSFONT_MAX_HEIGHT 64u

// Global system font instance
static sysfont_t g_sysfont = { .glyphs = 0,	   .numglyphs = 0,
			       .width = 0,	   .height = 0,
			       .bytesperglyph = 0, .unimap = 0,
			       .nunimap = 0,	   .replacement = 0,
			       .loaded = 0 };

// Direct-indexed Latin-1 lookup, filled from the Unicode table at load time.
// ASCII and Latin-1 cover the overwhelming majority of console output, and
// skipping the binary search for them keeps a full-screen redraw cheap.
static uint32_t g_latin1_map[256];

/* -------------------------------------------------------------------------
 * Unicode table -> sorted map
 * ---------------------------------------------------------------------- */

// Insertion sort by code point, keeping the first glyph seen for a duplicate.
// The tables in real fonts are a few hundred to a few thousand entries and
// arrive very nearly in order (glyph 0 upward, code points ascending within a
// glyph), which is the case insertion sort handles in close to linear time.
static uint32_t sysfont_sort_unimap(sysfont_unimap_t *map, uint32_t n)
{
	for (uint32_t i = 1; i < n; i++) {
		sysfont_unimap_t key = map[i];
		uint32_t j = i;
		while (j > 0 && map[j - 1].cp > key.cp) {
			map[j] = map[j - 1];
			j--;
		}
		map[j] = key;
	}

	// Collapse duplicates: a code point listed under several glyphs renders
	// as the lowest-numbered one, matching how the table is read top-down.
	uint32_t out = 0;
	for (uint32_t i = 0; i < n; i++) {
		if (out > 0 && map[out - 1].cp == map[i].cp) {
			if (map[i].glyph < map[out - 1].glyph)
				map[out - 1].glyph = map[i].glyph;
			continue;
		}
		map[out++] = map[i];
	}
	return out;
}

// Walk the raw PSF1 Unicode table, emitting one entry per (code point, glyph)
// pair.  Pass `map == NULL` to only count the pairs.
//
// Table grammar, per glyph: zero or more UCS-2 code points, then zero or more
// combining sequences each introduced by 0xFFFE, then 0xFFFF.  Combining
// sequences need composition the console does not do, so the values after a
// 0xFFFE are skipped -- the single code points before it are what a cell can
// render.  Values are little-endian, as everything in the format is.
static uint32_t sysfont_parse_unimap(const uint8_t *tab, const uint8_t *end,
				     uint32_t numglyph, sysfont_unimap_t *map,
				     uint32_t max)
{
	uint32_t count = 0;
	const uint8_t *p = tab;

	for (uint32_t glyph = 0; glyph < numglyph; glyph++) {
		int in_seq = 0;
		while (p + 1 < end) {
			uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
			p += 2;
			if (v == PSF1_SEPARATOR)
				break;
			if (v == PSF1_STARTSEQ) {
				in_seq = 1;
				continue;
			}
			if (in_seq)
				continue;
			if (map) {
				if (count >= max)
					return count;
				map[count].cp = v;
				map[count].glyph = glyph;
			}
			count++;
		}
		if (p + 1 >= end)
			break; /* table ended early; keep what was parsed */
	}
	return count;
}

/* -------------------------------------------------------------------------
 * Loading
 * ---------------------------------------------------------------------- */

// Load a PSF1 font file from the VFS.
int sysfont_load(const char *path)
{
	vfs_file_t *file = 0;
	struct kstat st;
	psf1_header_t header;
	uint8_t *glyphs = 0;
	uint8_t *rawtab = 0;
	sysfont_unimap_t *map = 0;
	uint32_t nmap = 0;
	long bytes_read;
	int ret;

	if (vfs_stat(path, &st) != ST_OK) {
		kprintf("sysfont: cannot stat %s\n", path);
		return -1;
	}

	ret = vfs_open(path, 0, &file);
	if (ret != ST_OK || !file) {
		kprintf("sysfont: failed to open %s (error %d)\n", path, ret);
		return -1;
	}

	bytes_read = vfs_read(file, &header, sizeof(header));
	if (bytes_read != (long)sizeof(header)) {
		kprintf("sysfont: failed to read PSF1 header from %s\n", path);
		goto fail;
	}

	if (header.magic[0] != PSF1_MAGIC0 || header.magic[1] != PSF1_MAGIC1) {
		kprintf("sysfont: %s is not a PSF1 font (magic %02x %02x)\n",
			path, header.magic[0], header.magic[1]);
		goto fail;
	}
	if (header.charsize == 0 || header.charsize > SYSFONT_MAX_HEIGHT) {
		kprintf("sysfont: unsupported PSF1 glyph height %u\n",
			header.charsize);
		goto fail;
	}

	// PSF1 is always 8 pixels wide, one byte per row, so the glyph size in
	// bytes and the height in pixels are the same number.
	uint32_t numglyph = (header.mode & PSF1_MODE512) ? 512u : 256u;
	uint32_t bytesperglyph = header.charsize;
	uint32_t height = header.charsize;

	uint64_t glyph_bytes = (uint64_t)numglyph * (uint64_t)bytesperglyph;
	if ((uint64_t)sizeof(header) + glyph_bytes > st.st_size) {
		kprintf("sysfont: %s truncated (need %llu bytes of glyph data)\n",
			path, (unsigned long long)glyph_bytes);
		goto fail;
	}

	glyphs = kalloc((size_t)glyph_bytes);
	if (!glyphs) {
		kprintf("sysfont: out of memory for %llu bytes of glyphs\n",
			(unsigned long long)glyph_bytes);
		goto fail;
	}

	bytes_read = vfs_read(file, glyphs, (long)glyph_bytes);
	if (bytes_read != (long)glyph_bytes) {
		kprintf("sysfont: failed to read PSF1 glyphs (read %ld of %llu)\n",
			bytes_read, (unsigned long long)glyph_bytes);
		goto fail;
	}

	// Unicode table, if the font carries one.
	uint64_t tab_off = (uint64_t)sizeof(header) + glyph_bytes;
	if ((header.mode & (PSF1_MODEHASTAB | PSF1_MODEHASSEQ)) &&
	    tab_off < st.st_size) {
		uint64_t tab_len = st.st_size - tab_off;
		rawtab = kalloc((size_t)tab_len);
		if (!rawtab) {
			kprintf("sysfont: out of memory for Unicode table\n");
			goto fail;
		}
		bytes_read = vfs_read(file, rawtab, (long)tab_len);
		if (bytes_read != (long)tab_len) {
			kprintf("sysfont: failed to read Unicode table (read %ld of %llu)\n",
				bytes_read, (unsigned long long)tab_len);
			goto fail;
		}

		const uint8_t *tab_end = rawtab + tab_len;
		uint32_t pairs =
			sysfont_parse_unimap(rawtab, tab_end, numglyph, 0, 0);
		if (pairs) {
			map = kalloc((size_t)pairs * sizeof(*map));
			if (!map) {
				kprintf("sysfont: out of memory for %u Unicode entries\n",
					pairs);
				goto fail;
			}
			nmap = sysfont_parse_unimap(rawtab, tab_end, numglyph,
						    map, pairs);
			nmap = sysfont_sort_unimap(map, nmap);
		}
		kfree(rawtab);
		rawtab = 0;
	}

	vfs_close(file);
	file = 0;

	/* Publish.  The previous font's storage is deliberately not freed: the
	 * renderer reads g_sysfont.glyphs on every drawn cell without taking a
	 * lock, so releasing it here would be a use-after-free against a redraw
	 * in flight on another CPU.  Reloading is a boot-time event bounded by
	 * the number of root devices probed, so the retained bytes are a font or
	 * two -- a trade the alternative (locking the glyph fetch) is not worth.
	 */
	g_sysfont.glyphs = glyphs;
	g_sysfont.numglyphs = numglyph;
	g_sysfont.width = SYSFONT_WIDTH;
	g_sysfont.height = height;
	g_sysfont.bytesperglyph = bytesperglyph;
	g_sysfont.unimap = map;
	g_sysfont.nunimap = nmap;

	// Latin-1 fast path.  Without a Unicode table the font is by convention
	// indexed by code point directly, which is what the identity fill gives.
	for (uint32_t i = 0; i < 256; i++)
		g_latin1_map[i] = (nmap == 0 && i < numglyph) ?
					  i :
					  SYSFONT_NO_GLYPH;
	for (uint32_t i = 0; i < nmap; i++) {
		if (map[i].cp < 256 &&
		    g_latin1_map[map[i].cp] == SYSFONT_NO_GLYPH)
			g_latin1_map[map[i].cp] = map[i].glyph;
	}

	g_sysfont.loaded = 1;

	// Resolve the substitute for code points the font has no glyph for,
	// once, so a miss costs one lookup rather than three.
	uint32_t rep = sysfont_glyph_index(0xFFFD);
	if (rep == SYSFONT_NO_GLYPH)
		rep = sysfont_glyph_index('?');
	if (rep == SYSFONT_NO_GLYPH)
		rep = 0;
	g_sysfont.replacement = rep;

	kprintf("sysfont: loaded PSF1 font %s (%ux%u, %u glyphs, %u code points)\n",
		path, SYSFONT_WIDTH, height, numglyph, nmap);
	return 0;

fail:
	if (file)
		vfs_close(file);
	if (rawtab)
		kfree(rawtab);
	if (map)
		kfree(map);
	if (glyphs)
		kfree(glyphs);
	return -1;
}

// Check if system font is loaded
int sysfont_is_loaded(void)
{
	return g_sysfont.loaded;
}

// Get system font structure
const sysfont_t *sysfont_get(void)
{
	return g_sysfont.loaded ? &g_sysfont : NULL;
}

// Map a Unicode code point to a glyph index.
uint32_t sysfont_glyph_index(uint32_t codepoint)
{
	if (!g_sysfont.loaded)
		return SYSFONT_NO_GLYPH;

	if (codepoint < 256)
		return g_latin1_map[codepoint];

	if (g_sysfont.nunimap == 0) {
		// No Unicode table: glyphs are indexed by code point.
		return (codepoint < g_sysfont.numglyphs) ? codepoint :
							   SYSFONT_NO_GLYPH;
	}

	uint32_t lo = 0, hi = g_sysfont.nunimap;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		uint32_t cp = g_sysfont.unimap[mid].cp;
		if (cp == codepoint)
			return g_sysfont.unimap[mid].glyph;
		if (cp < codepoint)
			lo = mid + 1;
		else
			hi = mid;
	}
	return SYSFONT_NO_GLYPH;
}

// Get glyph bitmap for a Unicode code point
const uint8_t *sysfont_get_glyph(uint32_t codepoint)
{
	if (!g_sysfont.loaded || !g_sysfont.glyphs)
		return NULL;
	WARN_ON_ONCE(
		g_sysfont.bytesperglyph ==
		0); /* loaded font has zero bytesperglyph: glyph lookup is broken */

	uint32_t idx = sysfont_glyph_index(codepoint);
	if (idx == SYSFONT_NO_GLYPH || idx >= g_sysfont.numglyphs)
		idx = g_sysfont.replacement;
	if (idx >= g_sysfont.numglyphs)
		return NULL;

	return &g_sysfont.glyphs[(uint64_t)idx * g_sysfont.bytesperglyph];
}

// Get font width
uint32_t sysfont_get_width(void)
{
	return g_sysfont.loaded ? g_sysfont.width : 8;
}

// Get font height
uint32_t sysfont_get_height(void)
{
	return g_sysfont.loaded ? g_sysfont.height : 16;
}
