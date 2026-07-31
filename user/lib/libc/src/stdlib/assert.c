/*
 * __assert_fail - the out-of-line half of assert().
 *
 * assert() must not expand to calls of ordinary library functions.  Those names
 * live in the user's namespace, and a program is entitled to have its own
 * variable called `abort`, `stderr` or `fprintf`; when it does, the expansion
 * resolves to the local object and the assertion stops compiling.  That is not
 * hypothetical -- NetSurf's tree walker keeps a `bool abort` flag, and every
 * assert() in that function failed to compile with "called object 'abort' is
 * not a function or function pointer".
 *
 * So the macro calls exactly one function, and it is named in the reserved
 * double-underscore space where no conforming program may collide with it.
 * This is what the reference libcs do, and why the problem never appears there.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

void __assert_fail(const char *expr, const char *file, unsigned int line,
		   const char *func)
{
	/* Written straight to stderr, unbuffered, because the process is about
	 * to abort: anything sitting in a stdio buffer would be lost. */
	fprintf(stderr, "%s:%u: %s%sAssertion `%s' failed.\n",
		file ? file : "?", line, func ? func : "",
		func ? ": " : "", expr ? expr : "?");
	fflush(stderr);
	abort();
}
