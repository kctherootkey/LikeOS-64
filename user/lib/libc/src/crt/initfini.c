/*
 * Constructor and destructor arrays for the main executable.
 *
 * An ELF program's constructors -- functions marked
 * __attribute__((constructor)), and every C++ static initialiser -- are listed
 * in .init_array, and something has to walk that list before main() runs.  The
 * dynamic linker does it for shared libraries but deliberately skips the main
 * object (rtld_init_dso: `if (d->initialized || d->is_main) return;`), because
 * on a conventional system the C runtime start-up code does that one.  Ours did
 * not, so no executable's constructors had ever run.
 *
 * That fails silently, which is what makes it nasty: nothing reports an error,
 * the program simply starts with whatever state the constructors were supposed
 * to establish still missing.  NetSurf is the case that exposed it -- libnsfb
 * registers each display surface from a constructor, so with none of them
 * running the browser found no surfaces at all, picked a NULL name for the
 * default one and dereferenced it.
 *
 * The symbols come from the linker script, which already brackets both
 * sections.  They are weak so a program linked without either section still
 * links: start and end then compare equal and the loops do nothing.
 */
#include <stddef.h>

/* Entries are called with (argc, argv, envp).  Constructors written in C take
 * no arguments and ignore them; passing them is what the reference runtime
 * does, and some code does declare the three-argument form. */
typedef void (*init_fn_t)(int, char **, char **);
typedef void (*fini_fn_t)(void);

extern init_fn_t __preinit_array_start[] __attribute__((weak));
extern init_fn_t __preinit_array_end[] __attribute__((weak));
extern init_fn_t __init_array_start[] __attribute__((weak));
extern init_fn_t __init_array_end[] __attribute__((weak));
extern fini_fn_t __fini_array_start[] __attribute__((weak));
extern fini_fn_t __fini_array_end[] __attribute__((weak));

static int fini_done;

/* Run before main().  Called from crt1. */
void __libc_run_init_array(int argc, char **argv, char **envp)
{
	size_t i, n;

	/* .preinit_array first, as the standard requires: it exists precisely
	 * so a few things can run ahead of ordinary constructors. */
	if (__preinit_array_start && __preinit_array_end) {
		n = (size_t)(__preinit_array_end - __preinit_array_start);
		for (i = 0; i < n; i++)
			if (__preinit_array_start[i])
				__preinit_array_start[i](argc, argv, envp);
	}

	if (__init_array_start && __init_array_end) {
		n = (size_t)(__init_array_end - __init_array_start);
		for (i = 0; i < n; i++)
			if (__init_array_start[i])
				__init_array_start[i](argc, argv, envp);
	}
}

/* Run at exit, in REVERSE order -- a destructor must be able to rely on
 * everything constructed after it still being intact when it runs.
 *
 * Guarded against running twice: exit() calls this, and so does the return
 * path from main(), and a program that calls exit() from inside main would
 * otherwise tear everything down twice. */
void __libc_run_fini_array(void)
{
	size_t i;

	if (fini_done)
		return;
	fini_done = 1;

	if (__fini_array_start && __fini_array_end) {
		i = (size_t)(__fini_array_end - __fini_array_start);
		while (i > 0) {
			i--;
			if (__fini_array_start[i])
				__fini_array_start[i]();
		}
	}
}
