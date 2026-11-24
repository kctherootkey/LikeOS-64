// LikeOS Task Viewer -- what Start new task decides before it starts a
// program: whether the program needs a terminal to be seen at all.
//
// A program started from the desktop has no terminal of its own.  One that
// talks to the user through a terminal (top, a shell, an editor in text
// mode) started like a windowed program runs unseen; it needs the terminal
// the desktop uses for such programs around it -- the same one, started the
// same way, as the panel uses for a desktop entry with Terminal=true
// (taskbar.c): xterm -e PROGRAM ARGS..., the program's words passed on as
// they are, never through a shell.
//
// A program file says which kind it is by the libraries it needs: an ELF
// program that needs none of the windowing and toolkit libraries (the X
// client libraries, GTK, GL, Wayland, SDL, Qt, WebKit, ...) draws no
// window, so it is a terminal program.  Needs are followed through the
// libraries the program needs in turn: a program that keeps its user
// interface in a library of its own (mousepad in libmousepad) says so only
// there.  A script (#!) cannot tell; without a desktop entry to say
// otherwise it is run in the terminal, where whatever it prints can be
// read.
//
// Nothing here touches X or GTK; tests/test-launch.c tests all of it on the
// build host.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef TV_LAUNCH_H
#define TV_LAUNCH_H

#include <glib.h>

G_BEGIN_DECLS

/* The terminal the desktop runs terminal programs in, and the option after
 * which it takes the program and its arguments. */
#define TV_TERMINAL "xterm"
#define TV_TERMINAL_EXEC "-e"

typedef enum {
	TV_PROGRAM_OTHER = 0, /* not a program file this can read: run as is */
	TV_PROGRAM_WINDOWED,  /* an ELF program that needs a windowing or
			       * toolkit library */
	TV_PROGRAM_TERMINAL,  /* an ELF program that needs none of them */
	TV_PROGRAM_SCRIPT,    /* a script (#!) */
} TvProgramKind;

/* The libraries an ELF image needs (its DT_NEEDED entries), in order;
 * an empty list for a program linked statically.  NULL when the data is
 * not an ELF image or is damaged.  32- and 64-bit images of either byte
 * order.  g_strfreev(). */
char **tv_elf_needed(const guint8 *data, gsize len);

/* Whether a library (a DT_NEEDED name such as "libX11.so.6") is one of
 * the windowing and toolkit libraries. */
gboolean tv_lib_is_windowing(const char *soname);

/* The kind of the program in `data' by the libraries it names itself (the
 * start of the file is enough for a script; an ELF image needs all of
 * it). */
TvProgramKind tv_program_kind_of_data(const guint8 *data, gsize len);
/* The kind of the program file at `path', the libraries it needs looked up
 * by name in `libdirs' (NULL-terminated) and followed to the libraries they
 * need; a library that cannot be found is passed over.  TV_PROGRAM_OTHER
 * when the file cannot be read. */
TvProgramKind tv_program_kind_in(const char *path,
				 const char *const *libdirs);
/* The same with the system's library folder, where the dynamic loader
 * finds them. */
TvProgramKind tv_program_kind(const char *path);

/* Whether a program of this kind, with no desktop entry to say otherwise,
 * is run in the terminal. */
gboolean tv_program_kind_needs_terminal(TvProgramKind kind);

/* A desktop entry's Exec line as the words to run: split the way a shell
 * splits them, with the field codes (%f, %U, %i, ...) left out, as no
 * files are passed, and %% as a %.  NULL when the line has no program.
 * g_strfreev(). */
char **tv_exec_argv(const char *exec);

/* The words that run `argv' in the terminal: TV_TERMINAL TV_TERMINAL_EXEC
 * argv...  g_strfreev(). */
char **tv_terminal_argv(const char *const *argv);

G_END_DECLS

#endif
