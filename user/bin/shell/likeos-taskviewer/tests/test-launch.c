// LikeOS Task Viewer -- tests of what Start new task decides before it
// starts a program (src/tv-launch.c): the libraries an ELF program needs,
// which of them draw windows, the kind of a program file, and the words
// that run a desktop entry's command or run a program in the terminal.
//
// Copyright (C) 2026 The LikeOS Project

#include <string.h>

#include <glib/gstdio.h>

#include "../src/tv-launch.h"

/* ---- a small ELF image ---------------------------------------------------- */

typedef struct {
	GByteArray *b;
	gboolean is64, big;
} Img;

static void put(Img *im, gsize off, guint64 v, guint size)
{
	if (im->b->len < off + size)
		g_byte_array_set_size(im->b, (guint)(off + size));
	for (guint i = 0; i < size; i++) {
		guint shift = im->big ? 8 * (size - 1 - i) : 8 * i;

		im->b->data[off + i] = (guint8)(v >> shift);
	}
}

/* An executable that needs `libs' (NULL-terminated; NULL or empty for none),
 * with a dynamic section unless `is_static'.  Laid out as a linker would:
 * one loaded segment at 0x400000 over the whole file, the dynamic section
 * and its strings inside it. */
static GByteArray *make_elf(gboolean is64, gboolean big,
			    const char *const *libs, gboolean is_static)
{
	Img im = { g_byte_array_new(), is64, big };
	guint ehsize = is64 ? 64 : 52, phentsize = is64 ? 56 : 32;
	guint phnum = is_static ? 1 : 2, word = is64 ? 8 : 4;
	gsize phoff = ehsize, str_off = phoff + phnum * phentsize, dyn_off;
	GString *str = g_string_new("");
	GArray *name_at = g_array_new(FALSE, FALSE, sizeof(guint));
	guint64 base = 0x400000;
	gsize end, o;

	/* The names first, so the dynamic section can point at them. */
	g_string_append_c(str, '\0');
	for (const char *const *l = libs; l && *l; l++) {
		guint at = (guint)str->len;

		g_array_append_val(name_at, at);
		g_string_append(str, *l);
		g_string_append_c(str, '\0');
	}
	dyn_off = (str_off + str->len + 7) & ~(gsize)7;

	g_byte_array_set_size(im.b, (guint)ehsize);
	memset(im.b->data, 0, ehsize);
	memcpy(im.b->data, "\177ELF", 4);
	im.b->data[4] = is64 ? 2 : 1;
	im.b->data[5] = big ? 2 : 1;
	im.b->data[6] = 1;
	put(&im, 16, 2, 2); /* ET_EXEC */
	if (is64) {
		put(&im, 0x20, phoff, 8);
		put(&im, 0x36, phentsize, 2);
		put(&im, 0x38, phnum, 2);
	} else {
		put(&im, 0x1c, phoff, 4);
		put(&im, 0x2a, phentsize, 2);
		put(&im, 0x2c, phnum, 2);
	}

	/* The strings and the dynamic section. */
	put(&im, str_off + str->len - 1, 0, 1);
	memcpy(im.b->data + str_off, str->str, str->len);
	o = dyn_off;
	if (!is_static) {
		for (guint i = 0; i < name_at->len; i++) {
			put(&im, o, 1, word); /* DT_NEEDED */
			put(&im, o + word, g_array_index(name_at, guint, i),
			    word);
			o += 2 * word;
		}
		put(&im, o, 5, word); /* DT_STRTAB */
		put(&im, o + word, base + str_off, word);
		o += 2 * word;
		put(&im, o, 10, word); /* DT_STRSZ */
		put(&im, o + word, str->len, word);
		o += 2 * word;
		put(&im, o, 0, word); /* DT_NULL */
		put(&im, o + word, 0, word);
		o += 2 * word;
	}
	end = o > str_off + str->len ? o : str_off + str->len;
	g_byte_array_set_size(im.b, (guint)end);

	/* PT_LOAD over everything; PT_DYNAMIC over the dynamic section. */
	for (guint i = 0; i < phnum; i++) {
		gsize p = phoff + i * phentsize;
		guint64 type = i == 0 ? 1 : 2;
		guint64 off = i == 0 ? 0 : dyn_off;
		guint64 size = i == 0 ? end : o - dyn_off;

		if (is64) {
			put(&im, p, type, 4);
			put(&im, p + 8, off, 8);
			put(&im, p + 16, base + off, 8);
			put(&im, p + 24, base + off, 8);
			put(&im, p + 32, size, 8);
			put(&im, p + 40, size, 8);
		} else {
			put(&im, p, type, 4);
			put(&im, p + 4, off, 4);
			put(&im, p + 8, base + off, 4);
			put(&im, p + 12, base + off, 4);
			put(&im, p + 16, size, 4);
			put(&im, p + 20, size, 4);
		}
	}
	g_string_free(str, TRUE);
	g_array_unref(name_at);
	return im.b;
}

static void check_needed(GByteArray *b, const char *const *want)
{
	char **got = tv_elf_needed(b->data, b->len);

	g_assert_nonnull(got);
	g_assert_cmpuint(g_strv_length(got), ==,
			 want ? g_strv_length((char **)want) : 0);
	for (guint i = 0; want && want[i]; i++)
		g_assert_cmpstr(got[i], ==, want[i]);
	g_strfreev(got);
}

static void test_elf_needed(void)
{
	static const char *const top[] = { "libc.so", "ld-likeos.so", NULL };
	static const char *const gui[] = { "libgtk-3.so.0", "libgdk-3.so.0",
					   "libglib-2.0.so.0", "libc.so", NULL };

	for (int is64 = 0; is64 < 2; is64++)
		for (int big = 0; big < 2; big++) {
			GByteArray *b = make_elf(is64, big, top, FALSE);

			check_needed(b, top);
			g_assert_cmpint(tv_program_kind_of_data(b->data, b->len),
					==, TV_PROGRAM_TERMINAL);
			g_byte_array_unref(b);

			b = make_elf(is64, big, gui, FALSE);
			check_needed(b, gui);
			g_assert_cmpint(tv_program_kind_of_data(b->data, b->len),
					==, TV_PROGRAM_WINDOWED);
			g_byte_array_unref(b);

			/* Linked statically: needs nothing, draws nothing. */
			b = make_elf(is64, big, NULL, TRUE);
			check_needed(b, NULL);
			g_assert_cmpint(tv_program_kind_of_data(b->data, b->len),
					==, TV_PROGRAM_TERMINAL);
			g_byte_array_unref(b);

			/* A dynamic section that needs nothing. */
			b = make_elf(is64, big, NULL, FALSE);
			check_needed(b, NULL);
			g_byte_array_unref(b);
		}
}

static void test_elf_damaged(void)
{
	static const char *const libs[] = { "libX11.so.6", "libc.so", NULL };
	GByteArray *b = make_elf(TRUE, FALSE, libs, FALSE);
	guint8 *copy;

	/* Cut short anywhere: never read past the end, never a wrong list.
	 * Cut inside the headers it is not an image at all; cut later it may
	 * still parse, but only to the right answer. */
	for (gsize n = 0; n < b->len; n++) {
		char **got;

		copy = g_memdup2(b->data, n);
		got = tv_elf_needed(copy, n);
		if (got) {
			for (guint i = 0; got[i]; i++)
				g_assert_cmpstr(got[i], ==, libs[i]);
			g_strfreev(got);
		}
		g_free(copy);
	}

	/* Not an image. */
	g_assert_null(tv_elf_needed((const guint8 *)"hello", 5));
	g_assert_null(tv_elf_needed(NULL, 0));
	copy = g_memdup2(b->data, b->len);
	copy[4] = 7; /* no such class */
	g_assert_null(tv_elf_needed(copy, b->len));
	g_free(copy);

	/* A name outside the strings. */
	copy = g_memdup2(b->data, b->len);
	for (gsize o = 64 + 2 * 56; o + 16 <= b->len; o += 8) {
		guint64 tag = 0, val = 0;

		memcpy(&tag, copy + o, 8);
		if (tag == 1) {
			memcpy(&val, copy + o + 8, 8);
			val += 100000;
			memcpy(copy + o + 8, &val, 8);
			break;
		}
	}
	g_assert_null(tv_elf_needed(copy, b->len));
	g_assert_cmpint(tv_program_kind_of_data(copy, b->len), ==,
			TV_PROGRAM_OTHER);
	g_free(copy);
	g_byte_array_unref(b);
}

static void test_windowing(void)
{
	static const char *const yes[] = {
		"libX11.so.6",	   "libX11-xcb.so.1", "libxcb.so.1",
		"libxcb-shm.so.0", "libgtk-3.so.0",   "libgtk-x11-2.0.so.0",
		"libgdk-3.so.0",   "libGL.so.1",      "libGLX.so.0",
		"libGLESv2.so.2",  "libEGL.so.1",     "libOpenGL.so.0",
		"libwayland-client.so.0", "libSDL2-2.0.so.0", "libQt5Core.so.5",
		"libQt6Widgets.so.6", "libwebkit2gtk-4.1.so.0", "libXt.so.6",
		"libXaw.so.7",	   "/usr/lib/libX11.so.6",
	};
	static const char *const no[] = {
		"libc.so",	    "ld-likeos.so",	"libglib-2.0.so.0",
		"libgio-2.0.so.0", "libncursesw.so.6", "libgdk_pixbuf-2.0.so.0",
		"libcairo.so.2",    "libpng16.so.16",	"libz.so.1",
		"libreadline.so.8", "libdrm.so.2",	"libxml2.so.2",
		"libgobject-2.0.so.0", "libgl.so", "",
	};

	for (guint i = 0; i < G_N_ELEMENTS(yes); i++)
		g_assert_true(tv_lib_is_windowing(yes[i]));
	for (guint i = 0; i < G_N_ELEMENTS(no); i++)
		g_assert_false(tv_lib_is_windowing(no[i]));
	g_assert_false(tv_lib_is_windowing(NULL));
}

static void test_kinds(void)
{
	static const char sh[] = "#!/bin/sh\necho hi\n";
	static const char text[] = "just some words\n";

	g_assert_cmpint(tv_program_kind_of_data((const guint8 *)sh, strlen(sh)),
			==, TV_PROGRAM_SCRIPT);
	g_assert_cmpint(tv_program_kind_of_data((const guint8 *)text,
						strlen(text)),
			==, TV_PROGRAM_OTHER);
	g_assert_cmpint(tv_program_kind_of_data(NULL, 0), ==, TV_PROGRAM_OTHER);

	g_assert_true(tv_program_kind_needs_terminal(TV_PROGRAM_TERMINAL));
	g_assert_true(tv_program_kind_needs_terminal(TV_PROGRAM_SCRIPT));
	g_assert_false(tv_program_kind_needs_terminal(TV_PROGRAM_WINDOWED));
	g_assert_false(tv_program_kind_needs_terminal(TV_PROGRAM_OTHER));
}

static void test_files(void)
{
	static const char *const gui[] = { "libX11.so.6", NULL };
	char *dir = g_dir_make_tmp("tv-launch-XXXXXX", NULL);
	char *elf = g_build_filename(dir, "prog", NULL);
	char *script = g_build_filename(dir, "script", NULL);
	char *missing = g_build_filename(dir, "missing", NULL);
	GByteArray *b = make_elf(TRUE, FALSE, gui, FALSE);

	g_assert_nonnull(dir);
	g_assert_true(g_file_set_contents(elf, (const char *)b->data,
					  (gssize)b->len, NULL));
	g_assert_true(g_file_set_contents(script, "#!/bin/sh\ntop\n", -1, NULL));
	g_assert_cmpint(tv_program_kind(elf), ==, TV_PROGRAM_WINDOWED);
	g_assert_cmpint(tv_program_kind(script), ==, TV_PROGRAM_SCRIPT);
	g_assert_cmpint(tv_program_kind(missing), ==, TV_PROGRAM_OTHER);
	g_assert_cmpint(tv_program_kind(dir), ==, TV_PROGRAM_OTHER);
	g_assert_cmpint(tv_program_kind(NULL), ==, TV_PROGRAM_OTHER);

	/* This test program itself, where the system shows it: an ELF
	 * program with libraries, none of which draws a window. */
	if (g_file_test("/proc/self/exe", G_FILE_TEST_EXISTS)) {
		char *self = g_file_read_link("/proc/self/exe", NULL);
		GMappedFile *f = self ? g_mapped_file_new(self, FALSE, NULL) :
					NULL;

		if (f) {
			char **needed = tv_elf_needed(
				(const guint8 *)g_mapped_file_get_contents(f),
				g_mapped_file_get_length(f));
			gboolean glib = FALSE;

			g_assert_nonnull(needed);
			for (char **n = needed; *n; n++)
				glib = glib || g_str_has_prefix(*n, "libglib-2.0");
			g_assert_true(glib);
			g_strfreev(needed);
			g_mapped_file_unref(f);
			g_assert_cmpint(tv_program_kind(self), ==,
					TV_PROGRAM_TERMINAL);
		}
		g_free(self);
	}

	g_unlink(elf);
	g_unlink(script);
	g_rmdir(dir);
	g_byte_array_unref(b);
	g_free(missing);
	g_free(script);
	g_free(elf);
	g_free(dir);
}

static void write_elf(const char *dir, const char *name,
		      const char *const *libs)
{
	GByteArray *b = make_elf(TRUE, FALSE, libs, FALSE);
	char *path = g_build_filename(dir, name, NULL);

	g_assert_true(g_file_set_contents(path, (const char *)b->data,
					  (gssize)b->len, NULL));
	g_byte_array_unref(b);
	g_free(path);
}

static void test_library_chain(void)
{
	char *dir = g_dir_make_tmp("tv-launch-XXXXXX", NULL);
	char *libdir = g_build_filename(dir, "lib", NULL);
	const char *const dirs[] = { "/nonexistent", libdir, NULL };
	const char *const none[] = { NULL };
	char *prog;
	static const char *const names[] = {
		"editor",   "libeditor.so.0", "libtoolkit.so.0", "tool",
		"libc.so",  "looper",	      "liba.so",	 "libb.so",
		"absolute",
	};

	g_assert_nonnull(dir);
	g_assert_cmpint(g_mkdir(libdir, 0700), ==, 0);
	/* A program whose window comes from its own library, two steps
	 * down. */
	write_elf(dir, "editor",
		  (const char *const[]){ "libeditor.so.0", "libc.so", NULL });
	write_elf(libdir, "libeditor.so.0",
		  (const char *const[]){ "libtoolkit.so.0", NULL });
	write_elf(libdir, "libtoolkit.so.0",
		  (const char *const[]){ "libc.so", "libgtk-3.so.0", NULL });
	write_elf(libdir, "libc.so", none);
	/* A text program with ordinary libraries. */
	write_elf(dir, "tool", (const char *const[]){ "libc.so", NULL });
	/* Libraries that need each other. */
	write_elf(dir, "looper", (const char *const[]){ "liba.so", NULL });
	write_elf(libdir, "liba.so",
		  (const char *const[]){ "libb.so", "libmissing.so", NULL });
	write_elf(libdir, "libb.so", (const char *const[]){ "liba.so", NULL });

	prog = g_build_filename(dir, "editor", NULL);
	g_assert_cmpint(tv_program_kind_in(prog, dirs), ==,
			TV_PROGRAM_WINDOWED);
	/* Its libraries not found: only what it names itself counts. */
	g_assert_cmpint(tv_program_kind_in(prog, none), ==,
			TV_PROGRAM_TERMINAL);
	g_free(prog);
	prog = g_build_filename(dir, "tool", NULL);
	g_assert_cmpint(tv_program_kind_in(prog, dirs), ==,
			TV_PROGRAM_TERMINAL);
	g_free(prog);
	prog = g_build_filename(dir, "looper", NULL);
	g_assert_cmpint(tv_program_kind_in(prog, dirs), ==,
			TV_PROGRAM_TERMINAL);
	g_free(prog);
	/* A library named by its path. */
	{
		char *lib = g_build_filename(libdir, "libtoolkit.so.0", NULL);

		write_elf(dir, "absolute", (const char *const[]){ lib, NULL });
		prog = g_build_filename(dir, "absolute", NULL);
		g_assert_cmpint(tv_program_kind_in(prog, none), ==,
				TV_PROGRAM_WINDOWED);
		g_free(prog);
		g_free(lib);
	}

	for (guint i = 0; i < G_N_ELEMENTS(names); i++) {
		char *a = g_build_filename(dir, names[i], NULL);
		char *b = g_build_filename(libdir, names[i], NULL);

		g_unlink(a);
		g_unlink(b);
		g_free(a);
		g_free(b);
	}
	g_rmdir(libdir);
	g_rmdir(dir);
	g_free(libdir);
	g_free(dir);
}

static void check_argv(char **got, const char *const *want)
{
	if (!want) {
		g_assert_null(got);
		return;
	}
	g_assert_nonnull(got);
	g_assert_cmpuint(g_strv_length(got), ==,
			 g_strv_length((char **)want));
	for (guint i = 0; want[i]; i++)
		g_assert_cmpstr(got[i], ==, want[i]);
	g_strfreev(got);
}

static void test_exec_argv(void)
{
	check_argv(tv_exec_argv("top"), (const char *const[]){ "top", NULL });
	check_argv(tv_exec_argv("mousepad %U"),
		   (const char *const[]){ "mousepad", NULL });
	check_argv(tv_exec_argv("foo %i --x %c %k %F"),
		   (const char *const[]){ "foo", "--x", NULL });
	check_argv(tv_exec_argv("env A=b \"/opt/my app/run\" --file=%f"),
		   (const char *const[]){ "env", "A=b", "/opt/my app/run",
					  "--file=", NULL });
	check_argv(tv_exec_argv("printf 100%% \"\""),
		   (const char *const[]){ "printf", "100%", "", NULL });
	check_argv(tv_exec_argv("sh -c 'echo $HOME; ls'"),
		   (const char *const[]){ "sh", "-c", "echo $HOME; ls", NULL });
	check_argv(tv_exec_argv("%U"), NULL);
	check_argv(tv_exec_argv(""), NULL);
	check_argv(tv_exec_argv(NULL), NULL);
	check_argv(tv_exec_argv("\"unterminated"), NULL);
}

static void test_terminal_argv(void)
{
	/* The words pass through untouched: no shell sees them. */
	const char *const cmd[] = { "/usr/bin/top", "-d", "1",
				    "$(rm -rf ~); `x` 'q' \"y\"", NULL };

	check_argv(tv_terminal_argv(cmd),
		   (const char *const[]){ TV_TERMINAL, TV_TERMINAL_EXEC,
					  "/usr/bin/top", "-d", "1",
					  "$(rm -rf ~); `x` 'q' \"y\"", NULL });
	check_argv(tv_terminal_argv((const char *const[]){ "top", NULL }),
		   (const char *const[]){ "xterm", "-e", "top", NULL });
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/launch/elf-needed", test_elf_needed);
	g_test_add_func("/launch/elf-damaged", test_elf_damaged);
	g_test_add_func("/launch/windowing", test_windowing);
	g_test_add_func("/launch/kinds", test_kinds);
	g_test_add_func("/launch/files", test_files);
	g_test_add_func("/launch/library-chain", test_library_chain);
	g_test_add_func("/launch/exec-argv", test_exec_argv);
	g_test_add_func("/launch/terminal-argv", test_terminal_argv);
	return g_test_run();
}
