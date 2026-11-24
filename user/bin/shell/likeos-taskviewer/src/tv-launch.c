// LikeOS Task Viewer -- whether a program needs a terminal (see
// tv-launch.h).
//
// Copyright (C) 2026 The LikeOS Project

#include <string.h>

#include "tv-launch.h"

/* ---- ELF -------------------------------------------------------------------- */

enum {
	PT_LOAD_ = 1,
	PT_DYNAMIC_ = 2,
	DT_NULL_ = 0,
	DT_NEEDED_ = 1,
	DT_STRTAB_ = 5,
	DT_STRSZ_ = 10,
};

typedef struct {
	const guint8 *data;
	gsize len;
	gboolean is64;
	gboolean big;
} Elf;

/* An unsigned field of `size' bytes at `off'; FALSE when it lies outside
 * the image. */
static gboolean get(const Elf *e, guint64 off, guint size, guint64 *out)
{
	guint64 v = 0;

	if (off > e->len || size > e->len - off)
		return FALSE;
	for (guint i = 0; i < size; i++) {
		guint shift = e->big ? 8 * (size - 1 - i) : 8 * i;

		v |= (guint64)e->data[off + i] << shift;
	}
	*out = v;
	return TRUE;
}

/* A word: 8 bytes in a 64-bit image, 4 in a 32-bit one. */
static gboolean get_word(const Elf *e, guint64 off, guint64 *out)
{
	return get(e, off, e->is64 ? 8 : 4, out);
}

typedef struct {
	guint64 type, offset, vaddr, filesz;
} Phdr;

static gboolean get_phdr(const Elf *e, guint64 off, Phdr *p)
{
	if (e->is64)
		return get(e, off, 4, &p->type) &&
		       get(e, off + 8, 8, &p->offset) &&
		       get(e, off + 16, 8, &p->vaddr) &&
		       get(e, off + 32, 8, &p->filesz);
	return get(e, off, 4, &p->type) && get(e, off + 4, 4, &p->offset) &&
	       get(e, off + 8, 4, &p->vaddr) && get(e, off + 16, 4, &p->filesz);
}

/* The file offset of a virtual address, through the loaded segments. */
static gboolean vaddr_to_offset(const Elf *e, const Phdr *ph, guint n,
				guint64 vaddr, guint64 *off)
{
	for (guint i = 0; i < n; i++) {
		if (ph[i].type != PT_LOAD_ || vaddr < ph[i].vaddr ||
		    vaddr - ph[i].vaddr >= ph[i].filesz)
			continue;
		*off = ph[i].offset + (vaddr - ph[i].vaddr);
		return *off < e->len;
	}
	return FALSE;
}

char **tv_elf_needed(const guint8 *data, gsize len)
{
	Elf e = { data, len, FALSE, FALSE };
	guint64 phoff, phentsize, phnum, dyn_off = 0, dyn_size = 0;
	guint64 strtab = 0, strsz = 0, str_off;
	gboolean have_dyn = FALSE, have_strtab = FALSE;
	GArray *needed;
	GPtrArray *out;
	Phdr *ph;
	guint dyn_entsize;

	if (!data || len < 0x34 || memcmp(data, "\177ELF", 4) != 0)
		return NULL;
	if (data[4] != 1 && data[4] != 2)
		return NULL;
	if (data[5] != 1 && data[5] != 2)
		return NULL;
	e.is64 = data[4] == 2;
	e.big = data[5] == 2;

	if (e.is64) {
		if (!get(&e, 0x20, 8, &phoff) || !get(&e, 0x36, 2, &phentsize) ||
		    !get(&e, 0x38, 2, &phnum))
			return NULL;
		if (phentsize < 56)
			return NULL;
	} else {
		if (!get(&e, 0x1c, 4, &phoff) || !get(&e, 0x2a, 2, &phentsize) ||
		    !get(&e, 0x2c, 2, &phnum))
			return NULL;
		if (phentsize < 32)
			return NULL;
	}
	/* 0xffff: the real count is elsewhere; no program has that many. */
	if (phnum == 0xffff || phoff >= len ||
	    phnum * phentsize > len - phoff)
		return NULL;

	ph = g_new0(Phdr, phnum + 1);
	for (guint i = 0; i < phnum; i++) {
		if (!get_phdr(&e, phoff + i * phentsize, &ph[i])) {
			g_free(ph);
			return NULL;
		}
		if (ph[i].type == PT_DYNAMIC_ && !have_dyn) {
			dyn_off = ph[i].offset;
			dyn_size = ph[i].filesz;
			have_dyn = TRUE;
		}
	}
	out = g_ptr_array_new();
	if (!have_dyn) {
		/* Linked statically: it needs nothing. */
		g_free(ph);
		g_ptr_array_add(out, NULL);
		return (char **)g_ptr_array_free(out, FALSE);
	}

	/* The dynamic section: the names' offsets, and where they are. */
	dyn_entsize = e.is64 ? 16 : 8;
	needed = g_array_new(FALSE, FALSE, sizeof(guint64));
	for (guint64 o = dyn_off; o + dyn_entsize <= dyn_off + dyn_size;
	     o += dyn_entsize) {
		guint64 tag, val;

		if (!get_word(&e, o, &tag) ||
		    !get_word(&e, o + dyn_entsize / 2, &val))
			goto bad;
		if (tag == DT_NULL_)
			break;
		if (tag == DT_NEEDED_)
			g_array_append_val(needed, val);
		else if (tag == DT_STRTAB_) {
			strtab = val;
			have_strtab = TRUE;
		} else if (tag == DT_STRSZ_)
			strsz = val;
	}
	if (needed->len) {
		if (!have_strtab ||
		    !vaddr_to_offset(&e, ph, (guint)phnum, strtab, &str_off))
			goto bad;
		if (!strsz || strsz > len - str_off)
			strsz = len - str_off;
		for (guint i = 0; i < needed->len; i++) {
			guint64 at = g_array_index(needed, guint64, i);
			const char *s;
			gsize max;

			if (at >= strsz)
				goto bad;
			s = (const char *)data + str_off + at;
			max = strsz - at;
			if (!memchr(s, '\0', max))
				goto bad;
			g_ptr_array_add(out, g_strdup(s));
		}
	}
	g_array_unref(needed);
	g_free(ph);
	g_ptr_array_add(out, NULL);
	return (char **)g_ptr_array_free(out, FALSE);
bad:
	g_array_unref(needed);
	g_free(ph);
	g_ptr_array_set_free_func(out, g_free);
	g_ptr_array_unref(out);
	return NULL;
}

/* ---- the kinds of program ---------------------------------------------------- */

gboolean tv_lib_is_windowing(const char *soname)
{
	/* Name prefixes: the X client libraries and the X toolkits, GTK and
	 * GDK, the GL family, Wayland, and the other toolkits and game
	 * libraries that open windows. */
	static const char *const prefixes[] = {
		"libX11",	  "libxcb",	   "libXt.",	"libXm.",
		"libXaw",	  "libgtk-",	   "libgtk3",	"libgdk-",
		"libgdk3",	  "libGL",	   "libEGL",	"libOpenGL",
		"libwayland-",	  "libSDL",	   "libQt",	"libKF5",
		"libKF6",	  "libwebkit",	   "libfltk",	"libwx_",
		"libtk8",	  "libglfw",	   "libecore_x", "libevas",
		"libelementary", "libsfml-window", "liballegro", "libraylib",
	};
	const char *base;

	if (!soname)
		return FALSE;
	base = strrchr(soname, '/');
	base = base ? base + 1 : soname;
	for (guint i = 0; i < G_N_ELEMENTS(prefixes); i++)
		if (g_str_has_prefix(base, prefixes[i]))
			return TRUE;
	return FALSE;
}

TvProgramKind tv_program_kind_of_data(const guint8 *data, gsize len)
{
	char **needed;
	TvProgramKind kind = TV_PROGRAM_TERMINAL;

	if (!data)
		return TV_PROGRAM_OTHER;
	if (len >= 2 && data[0] == '#' && data[1] == '!')
		return TV_PROGRAM_SCRIPT;
	needed = tv_elf_needed(data, len);
	if (!needed)
		return TV_PROGRAM_OTHER;
	for (char **n = needed; *n; n++)
		if (tv_lib_is_windowing(*n))
			kind = TV_PROGRAM_WINDOWED;
	g_strfreev(needed);
	return kind;
}

/* The libraries a file needs; NULL when it is no ELF image (or cannot be
 * read).  `script' tells a script. */
static char **file_needed(const char *path, gboolean *script)
{
	GMappedFile *f;
	const guint8 *d;
	gsize len;
	char **needed = NULL;

	if (script)
		*script = FALSE;
	if (!path || !g_file_test(path, G_FILE_TEST_IS_REGULAR))
		return NULL;
	f = g_mapped_file_new(path, FALSE, NULL);
	if (!f)
		return NULL;
	d = (const guint8 *)g_mapped_file_get_contents(f);
	len = g_mapped_file_get_length(f);
	if (d && len >= 2 && d[0] == '#' && d[1] == '!') {
		if (script)
			*script = TRUE;
	} else if (d) {
		needed = tv_elf_needed(d, len);
	}
	g_mapped_file_unref(f);
	return needed;
}

static char *find_library(const char *name, const char *const *libdirs)
{
	if (strchr(name, '/'))
		return g_file_test(name, G_FILE_TEST_IS_REGULAR) ?
			       g_strdup(name) :
			       NULL;
	for (const char *const *d = libdirs; d && *d; d++) {
		char *p = g_build_filename(*d, name, NULL);

		if (g_file_test(p, G_FILE_TEST_IS_REGULAR))
			return p;
		g_free(p);
	}
	return NULL;
}

/* No program needs more libraries than this, counted with theirs. */
#define MAX_LIBRARIES 1024

TvProgramKind tv_program_kind_in(const char *path,
				 const char *const *libdirs)
{
	gboolean script;
	char **needed = file_needed(path, &script);
	GHashTable *seen;
	GQueue todo = G_QUEUE_INIT;
	TvProgramKind kind = TV_PROGRAM_TERMINAL;
	char *name;

	if (script)
		return TV_PROGRAM_SCRIPT;
	if (!needed)
		return TV_PROGRAM_OTHER;
	/* Breadth first: what the program names itself is decided before
	 * any library is read.  The queue's names are the set's. */
	seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	for (;;) {
		for (char **n = needed; *n; n++) {
			char *copy;

			if (tv_lib_is_windowing(*n)) {
				kind = TV_PROGRAM_WINDOWED;
				break;
			}
			if (g_hash_table_contains(seen, *n) ||
			    g_hash_table_size(seen) >= MAX_LIBRARIES)
				continue;
			copy = g_strdup(*n);
			g_hash_table_add(seen, copy);
			g_queue_push_tail(&todo, copy);
		}
		g_clear_pointer(&needed, g_strfreev);
		if (kind == TV_PROGRAM_WINDOWED)
			break;
		/* The next library there is to read. */
		while (!needed && (name = g_queue_pop_head(&todo))) {
			char *lib = find_library(name, libdirs);

			needed = lib ? file_needed(lib, NULL) : NULL;
			g_free(lib);
		}
		if (!needed)
			break;
	}
	g_queue_clear(&todo);
	g_hash_table_destroy(seen);
	return kind;
}

TvProgramKind tv_program_kind(const char *path)
{
	static const char *const libdirs[] = { "/lib", "/usr/lib", NULL };

	return tv_program_kind_in(path, libdirs);
}

gboolean tv_program_kind_needs_terminal(TvProgramKind kind)
{
	return kind == TV_PROGRAM_TERMINAL || kind == TV_PROGRAM_SCRIPT;
}

/* ---- the words to run ---------------------------------------------------------- */

char **tv_exec_argv(const char *exec)
{
	char **words = NULL;
	GPtrArray *out;
	int n = 0;

	if (!exec || !g_shell_parse_argv(exec, &n, &words, NULL))
		return NULL;
	out = g_ptr_array_new();
	for (int i = 0; i < n; i++) {
		GString *w = g_string_new(NULL);
		gboolean code_only = TRUE;

		for (const char *s = words[i]; *s; s++) {
			if (s[0] == '%' && s[1]) {
				if (s[1] == '%') {
					g_string_append_c(w, '%');
					code_only = FALSE;
				}
				/* Any other field code: no files, no icon,
				 * no name -- left out. */
				s++;
				continue;
			}
			g_string_append_c(w, *s);
			code_only = FALSE;
		}
		/* A word that was only field codes is gone; one that became
		 * empty otherwise ("") stays an argument. */
		if (code_only && words[i][0])
			g_string_free(w, TRUE);
		else
			g_ptr_array_add(out, g_string_free(w, FALSE));
	}
	g_strfreev(words);
	if (!out->len) {
		g_ptr_array_unref(out);
		return NULL;
	}
	g_ptr_array_add(out, NULL);
	return (char **)g_ptr_array_free(out, FALSE);
}

char **tv_terminal_argv(const char *const *argv)
{
	GPtrArray *out = g_ptr_array_new();

	g_ptr_array_add(out, g_strdup(TV_TERMINAL));
	g_ptr_array_add(out, g_strdup(TV_TERMINAL_EXEC));
	for (const char *const *a = argv; a && *a; a++)
		g_ptr_array_add(out, g_strdup(*a));
	g_ptr_array_add(out, NULL);
	return (char **)g_ptr_array_free(out, FALSE);
}
